/*
 *
 * Copyright (C) 2017  Leica Geosystems. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

/*
 * Simple frame buffer driver using the Altera SGDMA ip.
 * Only one minor device (= camera) at a time can wait for a capture.
 * There is only one DMA buffer per camera (no queueing).
 * There is only one DMA IRQ.
 * We could queue requests and only set the GO bit in the last DMA descriptor,
 * but we have to multiplex between the cameras anyway.
 */

/*
 * NOTE: driver is under development. It contains debug code and no locks
 * or semaphores.
 * XXX: remove minor device support and camera references
 */

/* define before device.h to get dev_dbg output */
#define DEBUG
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>
#include <linux/sched.h>	/* 'current' pointer */
#include <linux/dma-mapping.h>	/* dma_alloc_coherent() */
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/poll.h>
#include <linux/delay.h>	/* udelay() */
#include <linux/list.h>

#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>

#include <linux/lgs/colibri-edm-dev.h>
#include "msgdmahw.h"

#define N_CAMERAS	4

struct fb_allocs {
	struct col_fb_alloc fb_alloc;
	struct list_head list;
};

/* Device data used by this driver. */
struct colcam_dev {
	struct device *device;
	struct fb_allocs fb_allocs;
	struct col_capinfo capinfo;
};

static int colibri_edm_major;	/* dynamic major by default */
static struct cdev colibri_edm_cdev;
static unsigned int colibri_edm_irq;
static struct class *colibri_edm_class;
static wait_queue_head_t colibri_edm_waitq;
static int request_count = 0;
static int dma_fill_level;
static int irq_counter;
static spinlock_t spinlock;

static struct colcam_dev colcam_device_array[N_CAMERAS];
static struct colcam_dev *colcam_devices = &colcam_device_array[0];
static struct colcam_dev *current_colcam;

#define DRIVER_NAME "colibri_edm"

static struct msgdma_csr_regs *dma_csr;
static struct msgdma_desc_regs *dma_desc_regs;
static struct msgdma_response *dma_resp;

static void msgdma_reset(void)
{
	uint32_t value;
	int i;

	/* clear all status bits */
	writel(0x3ff, &dma_csr->status);
	/* reset dispatcher */
	writel(MSGDMA_CSR_RESET_DISP, &dma_csr->control);

	for (i = 0; i < 10000; i++) {
		value = readl(&dma_csr->status);
		if (!(value & MSGDMA_CSR_RESETTING))
			break;
		udelay(1);
	}
	if (i >= 10000)
		pr_warn("SGDMA resetting bit not cleared!\n");

	/* clear all status bits */
	writel(0x3ff, &dma_csr->status);

	pr_info("msgdma_reset: dma_csr->status 0x%x\n", readl(&dma_csr->status));
	pr_info("msgdma_reset: dma_csr->control 0x%x\n", readl(&dma_csr->control));
}

static void msgdma_disable_irq(void)
{
	uint32_t value;

	value = readl(&dma_csr->control);
	value &= ~MSGDMA_CSR_IRQ_EN;
	writel(value, &dma_csr->control);
}

static void msgdma_enable_irq(void)
{
	uint32_t value;

	value = readl(&dma_csr->control);
	value |= MSGDMA_CSR_IRQ_EN;
	writel(value, &dma_csr->control);
}

static int colibri_edm_open(struct inode *inode, struct file *filep)
{
	int ret;
	int camera;
	struct device *dev;
	struct colcam_dev *colcam;

	pr_info("%s: %d\n", __func__, iminor(inode));

	ret = 0;

	camera = iminor(inode) & 0x0f;

	filep->private_data = &colcam_devices[camera];

	dev = colcam_devices[camera].device;
	dev_dbg(dev, "%s: camera %d\n", __func__, camera);

	colcam = filep->private_data;
	init_waitqueue_head(&colibri_edm_waitq);
	spin_lock_init(&spinlock);

	/*
	 * XXX: workaround, reset DMA to empty DMA buffer FIFO
	 */
	msgdma_disable_irq();
	msgdma_reset();
	irq_counter = 0;
	dma_fill_level = 0;
	request_count = 0;
	msgdma_enable_irq();

	return ret;
}

static int colibri_edm_close(struct inode *inode, struct file *filep)
{
	size_t size;
	dma_addr_t dma_addr;
	void *vaddr;
	struct colcam_dev *colcam = filep->private_data;
	struct device *dev = colcam->device;
	struct fb_allocs *fb_allocs;
	struct fb_allocs *tmp;
	unsigned long flags;

	pr_info("%s: %d\n", __func__, iminor(inode));

	spin_lock_irqsave(&spinlock, flags);
	msgdma_disable_irq();
	spin_unlock_irqrestore(&spinlock, flags);

	/* go through list of allocated fb's for this minor and free them */
	list_for_each_entry_safe(fb_allocs, tmp, &colcam->fb_allocs.list,
			list) {

		size = fb_allocs->fb_alloc.size;
		dma_addr = fb_allocs->fb_alloc.offset;
		vaddr = fb_allocs->fb_alloc.vaddr;

		dma_free_coherent(dev, size, vaddr, dma_addr);
		/*
		dev_dbg(dev, "%s: freeing: size %d, vaddr %p, dma_addr %x\n",
			__func__, size, vaddr, dma_addr);
		*/
		list_del(&fb_allocs->list);
		kfree(fb_allocs);
	}

	filep->private_data = NULL;

	return 0;
}

static int ioctl_fb_alloc(struct colcam_dev *colcam, void __user *udata)
{
	int ret;
	struct col_fb_alloc col_fb_alloc;
	size_t size;
	dma_addr_t dma_addr;
	void *vaddr;
	struct device *dev = colcam->device;
	struct fb_allocs *fb_allocs;

	if (copy_from_user(&col_fb_alloc, udata, sizeof(col_fb_alloc)))
		return -EFAULT;

	size = col_fb_alloc.size;

	vaddr = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);

	if (!vaddr) {
		dev_err(dev, "memory alloc size %zd failed\n", size);
		return -ENOMEM;
	}

	*(unsigned int *)vaddr = 0x12345678;	/* test pattern */
	/*
	dev_dbg(dev, "dma mapped data %p is at %p (%zd)\n",
		(void *)dma_addr, vaddr, size);
	*/

	fb_allocs = kmalloc(sizeof(*fb_allocs), GFP_KERNEL);
	fb_allocs->fb_alloc.offset = (off_t) dma_addr;
	fb_allocs->fb_alloc.vaddr = vaddr;
	fb_allocs->fb_alloc.size = size;
	INIT_LIST_HEAD(&fb_allocs->list);
	list_add(&fb_allocs->list, &colcam->fb_allocs.list);

	col_fb_alloc.offset = dma_addr;
	col_fb_alloc.vaddr = vaddr;
	ret = copy_to_user(udata, &col_fb_alloc, sizeof(col_fb_alloc));

	return ret;
}

static int ioctl_mmap_to_phys(struct colcam_dev *colcam, void __user *udata)
{
	int ret;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long mmapped_addr;
	struct col_mmap_to_phys col_mmap_to_phys;

	if (copy_from_user(&col_mmap_to_phys, udata, sizeof(col_mmap_to_phys)))
		return -EFAULT;

	mmapped_addr = (unsigned long)col_mmap_to_phys.user_va;

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, mmapped_addr);
	up_read(&mm->mmap_sem);
	if (!vma)
		return -EINVAL;

	/* XXX: do more vma checks here */
	col_mmap_to_phys.phys = (void *) (vma->vm_pgoff << PAGE_SHIFT);

	ret = copy_to_user(udata, &col_mmap_to_phys, sizeof(col_mmap_to_phys));

	return ret;
}

/*
 * find matching allocation/mapping info for given dma_addr
 */
static struct col_fb_alloc *find_alloc_info(struct colcam_dev *colcam,
		                         uint32_t dma_addr)
{
	struct fb_allocs *fb_allocs;
	struct device *dev = colcam->device;

	list_for_each_entry(fb_allocs, &colcam->fb_allocs.list, list) {
		if (fb_allocs->fb_alloc.offset == dma_addr) {
			return &fb_allocs->fb_alloc;
		}
	}

	dev_err(dev, "invalid dma address 0x%x\n", dma_addr);
	return NULL;
}

static int ioctl_capture(struct colcam_dev *colcam, void __user *udata)
{
	uint32_t write_addr = (uint32_t) udata;
	/* struct device *dev = colcam->device; */
	struct msgdma_desc_regs dma_desc = {0};
	struct col_fb_alloc *fb_alloc_info;
	unsigned long flags;

	/*
	 * FIXME: return EBUSY if a capture was already started for
	 * this or another minor device
	 */

	/* find mapping info for given write_addr (dma_addr) */
	fb_alloc_info = find_alloc_info(colcam, write_addr);
	if (!fb_alloc_info)
		return -EINVAL;

	/*
	 * We have only one irq handler for the cameras.
	 * Set the current for the irq handler.
	 */
	current_colcam = colcam;

	/*
	dev_dbg(dev, "%s: status 0x%x, control 0x%x\n", __func__,
		readl(&dma_csr->status), readl(&dma_csr->control));
	*/

	/* start DMA */
	dma_desc.write_addr = write_addr;
	/*
	dev_dbg(dev, "%s: write_addr 0x%x, request_count %d\n",
			__func__, dma_desc.write_addr, request_count);
	*/
	dma_desc.length = fb_alloc_info->size;
	/* Either the length or the End of Packet will end the transfer. */
	dma_desc.control |= MSGDMA_DESC_END_ON_EOP;
	dma_desc.control |= MSGDMA_DESC_END_ON_LEN;
	dma_desc.control |= MSGDMA_DESC_TX_IRQ_EN;
	dma_desc.control |= MSGDMA_DESC_EARLY_TERM_IRQ_EN;
	dma_desc.control |= MSGDMA_DESC_ERR_IRQ_EN;
	dma_desc.control |= MSGDMA_DESC_GO;

	writel(dma_desc.read_addr, &dma_desc_regs->read_addr);
	writel(dma_desc.write_addr, &dma_desc_regs->write_addr);
	writel(dma_desc.length, &dma_desc_regs->length);
	spin_lock_irqsave(&spinlock, flags);
	writel(dma_desc.control, &dma_desc_regs->control);
	request_count++;
	spin_unlock_irqrestore(&spinlock, flags);

	return 0;
}

static int ioctl_capinfo(struct colcam_dev *colcam, void __user *udata)
{
	int ret;
	struct col_capinfo capinfo;

	capinfo.bytesused = colcam->capinfo.bytesused;
	ret = copy_to_user(udata, &capinfo, sizeof(capinfo));

	return ret;
}

static long colibri_edm_ioctl(struct file *filep, unsigned int cmd, unsigned long data)
{
	int ret = -ENOSYS;
	struct colcam_dev *colcam = filep->private_data;
	void __user *udata = (void __user *) data;

	switch (cmd) {
	case COL_IOC_FB_ALLOC:
		ret = ioctl_fb_alloc(colcam, udata);
		break;

	case COL_IOC_MMAP_TO_PHYS:
		ret = ioctl_mmap_to_phys(colcam, udata);
		break;

	case COL_IOC_CAPTURE:
		ret = ioctl_capture(colcam, udata);
		break;

	case COL_IOC_CAPINFO:
		ret = ioctl_capinfo(colcam, udata);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static unsigned int colibri_edm_poll(struct file *filep, poll_table *wait)
{
	struct colcam_dev *colcam = filep->private_data;
	struct device *dev = colcam->device;
	unsigned int mask = 0;
	unsigned long flags;

	//dev_dbg(dev, "%s, poll_table %p, poll_does_not_wait %d\n",
			//__func__, wait, poll_does_not_wait(wait));
	poll_wait(filep, &colibri_edm_waitq, wait);

	/*
	dev_dbg(dev, "%s irq_counter %d, request_count %d, dma_fill_level %d\n",
			__func__, irq_counter, request_count, dma_fill_level);
	*/

	spin_lock_irqsave(&spinlock, flags);

	if (dma_fill_level > 1) {
		dev_dbg(dev, "%s irq_counter %d, request_count %d, "
				"dma_fill_level %d\n",
			__func__, irq_counter, request_count, dma_fill_level);
	}

	if (dma_fill_level > 0) {
		--dma_fill_level;
		mask = POLLIN | POLLRDNORM;
	}

	spin_unlock_irqrestore(&spinlock, flags);

	return mask;
}

static int colibri_edm_mmap(struct file *filep, struct vm_area_struct *vma)
{
	int ret;
	/*
	struct colcam_dev *colcam = filep->private_data;
	struct device *dev = colcam->device;

	dev_dbg(dev, "%s: vm_start %lx, vm_pgoff %lx,\n", __func__,
			vma->vm_start, vma->vm_pgoff);
	*/
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	/* XXX: consider dma_mmap_coherent() */
	ret = remap_pfn_range(vma,
			       vma->vm_start,
			       vma->vm_pgoff,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);
	return ret;
}

static irqreturn_t colibri_edm_intr(int irq, void *dev_id)
{
	uint32_t value;
        uint32_t resp_status = 0;
        uint32_t bytes_transferred = 0;
	unsigned int	i;

	spin_lock(&spinlock);
	/* clear DMA IRQ */
	value = MSGDMA_CSR_IRQ;
	writel(value, &dma_csr->status);

	value = readl(&dma_csr->resp_fill_level);
	if (value & 0xffff) {
		if (value > 1)
			pr_info("resp_fill_level %d\n", value);
		for (i = 0; i < value; i ++) {
			/*
			 * CAUTION: reading status pops the response and a read
			 * on the emtpy FIFO will hang the complete system.
			 */
			bytes_transferred = readl(&dma_resp->bytes_transferred);
			resp_status = readl(&dma_resp->status);
		}
	}

#if 0
	/*
	 * XXX: it seems that bytes_transferred is always 0, if no EOP
	 * was generated.
	 */
	if ((resp_status & 0xff) || (bytes_transferred == 0))
#else
	if ((resp_status & 0xff))
#endif
		dev_err(current_colcam->device,
				"resp_status %08X, bytes_transferred %08X\n",
				resp_status, bytes_transferred);

	current_colcam->capinfo.bytesused = bytes_transferred;

	/*
	pr_info("%s: status 0x%x, control 0x%x, resp 0x%x\n", __func__,
		readl(&dma_csr->status), readl(&dma_csr->control), resp_status);
	*/

	dma_fill_level += value;
	irq_counter += value;
	wake_up_interruptible(&colibri_edm_waitq);
	// pr_info("%s: irq_counter %d\n", __func__, irq_counter);
	spin_unlock(&spinlock);

	return IRQ_HANDLED;
}

struct file_operations colibri_edm_fileops = {
	.owner =  THIS_MODULE,
//	.read =     colibri_edm_read,
//	.write =    colibri_edm_write,
	.unlocked_ioctl = colibri_edm_ioctl,
	.open = colibri_edm_open,
	.release = colibri_edm_close,
	.poll = colibri_edm_poll,
	.mmap    = colibri_edm_mmap,
};

/* Allocate and create devices in /dev */
static int __init init_chrdev (void)
{
	int dev_minor = 0;
	int i;
	int ret;
	struct device *device;
	dev_t cdev_num;

	/* request major number for camera devices */
	ret = alloc_chrdev_region(&cdev_num, dev_minor, N_CAMERAS,
			          DRIVER_NAME);
	if (ret < 0) {
		pr_err("can't alloc chrdev %d\n", ret);
		return ret;
	}

	colibri_edm_major = MAJOR(cdev_num);

	/* Create a sysfs class. */
	colibri_edm_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(colibri_edm_class)) {
		pr_err("%s: can't create class\n", __func__);
		goto fail_class;
	}

	/* Register character device. */
	cdev_init(&colibri_edm_cdev, &colibri_edm_fileops);
	colibri_edm_cdev.owner = THIS_MODULE;
	colibri_edm_cdev.ops = &colibri_edm_fileops;
	ret = cdev_add(&colibri_edm_cdev, cdev_num, N_CAMERAS);
	if (ret) {
		pr_err("Error %d adding colibri_edm (%d, %d)",
			ret, colibri_edm_major, dev_minor);
		goto fail_add;
	}

	/* create device nodes under /dev using udev */
	for (i = 0; i < N_CAMERAS; i++) {
		device = device_create(colibri_edm_class, NULL,
				MKDEV(colibri_edm_major, i), colcam_devices + i,
				DRIVER_NAME "%d", i);
		if (IS_ERR(device)) {
			pr_err("Can't create device\n");
			goto fail_dev_create;
		}
		colcam_devices[i].device = device;
		dma_set_coherent_mask(device, DMA_BIT_MASK(32));
		INIT_LIST_HEAD(&colcam_devices[i].fb_allocs.list);
	}

	return 0;

	/* ERROR HANDLING */
fail_dev_create:
	for (i = 0; i < N_CAMERAS; i++)
		device_destroy(colibri_edm_class, MKDEV(colibri_edm_major, i));
	cdev_del(&colibri_edm_cdev);

fail_add:
	class_destroy(colibri_edm_class);

fail_class:
	/* free the dynamically allocated character device node */
	unregister_chrdev_region(cdev_num, N_CAMERAS/*count*/);

	return -1;
}

static int colibri_edm_remove(struct platform_device *pdev)
{
	int i;

	msgdma_disable_irq();
	msgdma_reset();

	pm_runtime_disable(&pdev->dev);

	for (i = 0; i < N_CAMERAS; i++)
		device_destroy(colibri_edm_class, MKDEV(colibri_edm_major, i));
	class_destroy(colibri_edm_class);
	cdev_del(&colibri_edm_cdev);
	unregister_chrdev_region(MKDEV(colibri_edm_major, 0), N_CAMERAS);

	if (colibri_edm_irq)
		free_irq(colibri_edm_irq, NULL);

	return 0;
}

/*
 * Like the kernel platform_get_resource, but with debug info.
 */
static struct resource *get_resource(struct platform_device *pdev,
		unsigned int type, unsigned int num)
{
	int i;

	pr_info("%s: type %x\n", __func__, type);
	for (i = 0; i < pdev->num_resources; i++) {
		struct resource *r = &pdev->resource[i];
		pr_info("res %d:  start %x, end %x, flags %lx\n", i,
				r->start, r->end, r->flags);

		pr_info("resource_type %lx, num %u\n", resource_type(r), num);

		if (resource_type(r) == IORESOURCE_MEM && num-- == 0)
			return r;
	}

	return NULL;
}

static int colibri_edm_probe(struct platform_device *pdev)
{
	int ret;
	int irq;
	struct resource *res;
	struct resource *region;

	ret = init_chrdev();
	if (ret) {
		goto fail_chrdev_init;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "failed to get IRQ\n");
		ret = irq;
		goto fail_chrdev_init;
	}

	/*
	 * map CSR
	 */
	res = get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		dev_err(&pdev->dev, "failed to get IORESOURCE_MEM\n");

		ret = -ENODEV;
		goto fail_mapping;
	}

	region = devm_request_mem_region(&pdev->dev, res->start,
			resource_size(res), pdev->name);
	if (region == NULL) {
		dev_err(&pdev->dev, "failed to get mem region\n");
		ret = -EBUSY;
		goto fail_mapping;
	}

	dma_csr = devm_ioremap_nocache(&pdev->dev, res->start,
					resource_size(res));
	if (dma_csr == NULL) {
		dev_err(&pdev->dev, "devm_ioremap_nocache failed\n");
		ret = -ENOMEM;
		goto fail_mapping;
	}

	/*
	*/
	pr_info("dma_csr->status 0x%x, control 0x%x\n",
		readl(&dma_csr->status), readl(&dma_csr->control));

	/*
	 * sanity check: do not continue if no device is present,
	 * otherwise we might end up in endless irqs or device hang
	 * if no FPGA IP is present or the FPGA is not configured.
	 * The irq line seems to be active if the FPGA is unconfigured.
	 */
	if (dma_csr->control == ~0) {
		dev_err(&pdev->dev, "no EDM (FPGA) device present\n");
		ret = -ENODEV;
		goto fail_mapping;
	}

	ret = request_irq(irq, colibri_edm_intr, 0 /*flags*/,
			  DRIVER_NAME, NULL);
	if (ret) {
		dev_err(&pdev->dev, "failed to register IRQ %d\n", irq);
		goto fail_mapping;
	}
	colibri_edm_irq = irq;

	/*
	 * map descriptor register (write-only)
	 */
	res = get_resource(pdev, IORESOURCE_MEM, 1);
	if (res == NULL) {
		dev_err(&pdev->dev, "failed to get IORESOURCE_MEM for descr\n");

		ret = -ENODEV;
		goto fail_mapping;
	}

	region = devm_request_mem_region(&pdev->dev, res->start,
			resource_size(res), pdev->name);
	if (region == NULL) {
		dev_err(&pdev->dev, "failed to get mem region\n");
		ret = -EBUSY;
		goto fail_mapping;
	}

	dma_desc_regs = devm_ioremap_nocache(&pdev->dev, res->start,
						resource_size(res));
	if (dma_desc_regs == NULL) {
		dev_err(&pdev->dev, "devm_ioremap_nocache failed for descr\n");
		ret = -ENOMEM;
		goto fail_mapping;
	}

	/*
	 * map response register (read-only)
	 */
	res = get_resource(pdev, IORESOURCE_MEM, 2);
	if (res == NULL) {
		dev_err(&pdev->dev, "failed to get IORESOURCE_MEM for resp\n");

		ret = -ENODEV;
		goto fail_mapping;
	}

	region = devm_request_mem_region(&pdev->dev, res->start,
			resource_size(res), pdev->name);
	if (region == NULL) {
		dev_err(&pdev->dev, "failed to get mem region\n");
		ret = -EBUSY;
		goto fail_mapping;
	}

	dma_resp = devm_ioremap_nocache(&pdev->dev, res->start,
						resource_size(res));
	if (dma_resp == NULL) {
		dev_err(&pdev->dev, "devm_ioremap_nocache failed for descr\n");
		ret = -ENOMEM;
		goto fail_mapping;
	}

	init_waitqueue_head(&colibri_edm_waitq);
	dma_fill_level = 0;
	request_count = 0;
	msgdma_reset();

	return 0;

fail_mapping:
	colibri_edm_remove(pdev);
fail_chrdev_init:
	return ret;
}

static int colibri_edm_runtime_nop(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops colibri_edm_dev_pm_ops = {
	.runtime_suspend = colibri_edm_runtime_nop,
	.runtime_resume = colibri_edm_runtime_nop,
};

#ifdef CONFIG_OF
static struct of_device_id colibri_edm_match[] = {
	{ .compatible = "colibri-edm" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, colibri_edm_match);
module_param_string(of_id, colibri_edm_match[0].compatible, 128, 0);
MODULE_PARM_DESC(of_id, "Openfirmware id of the device to be handled by colibri_edm");
#else
# define colibri_edm_match NULL
#endif

static struct platform_driver colibri_edm_driver = {
	.probe = colibri_edm_probe,
	.remove = colibri_edm_remove,
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.pm = &colibri_edm_dev_pm_ops,
		.of_match_table = colibri_edm_match,
	},
};

module_platform_driver(colibri_edm_driver);

MODULE_AUTHOR("Michael Brandt");
MODULE_DESCRIPTION("colibri EDM driver.");
MODULE_LICENSE("GPL v2");

/* vim: set sw=8 ts=8 noexpandtab: */
