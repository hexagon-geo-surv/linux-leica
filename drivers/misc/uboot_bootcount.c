/*
 * This driver gives access(read/write) to the bootcounter used by u-boot.
 * Access is supported via sysFS.
 *
 * Copyright 2008 DENX Software Engineering GmbH
 * Author: Heiko Schocher <hs@denx.de>
 * Based on work from: Steffen Rumler  <Steffen.Rumler@siemens.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#define UBOOT_BOOTCOUNT_NAME "bootcount"

#define	UBOOT_BOOTCOUNT_MAGIC_OFFSET	0x04	/* offset of magic number */
#define	UBOOT_BOOTCOUNT_MAGIC		0xB001C041 /* magic number value */

static void __iomem *mem;

/* helper for the sysFS */
static int show_str_bootcount(struct device *device,
				struct device_attribute *attr,
				char *buf)
{
	unsigned long counter;

	counter = be32_to_cpu(readl(mem));

	return sprintf(buf, "%lu\n", counter);
}

static int store_str_bootcount(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	int r;
	u32 value;
	unsigned long magic;

	magic = be32_to_cpu(readl(mem + UBOOT_BOOTCOUNT_MAGIC_OFFSET));
	if (magic != UBOOT_BOOTCOUNT_MAGIC)
		return -EINVAL;

	r = kstrtou32(buf, 0, &value);
	if (r < 0)
		return -EINVAL;

	writel(cpu_to_be32(value), mem);

	return count;
}

static DEVICE_ATTR(bootcount, S_IWUSR | S_IRUGO, show_str_bootcount,
		store_str_bootcount);

static const struct file_operations bootcount_fops = {
	.owner = THIS_MODULE,
};

static struct miscdevice bootcount_miscdev = {
	MISC_DYNAMIC_MINOR,
	UBOOT_BOOTCOUNT_NAME,
	&bootcount_fops
};


static int bootcount_probe(struct platform_device *ofdev)
{
	struct device_node *np = of_node_get(ofdev->dev.of_node);
	unsigned long magic;

	mem = of_iomap(np, 0);
	if (mem == NULL) {
		dev_err(&ofdev->dev, "couldnt map register.\n");
		return -ENODEV;
	}
	magic = be32_to_cpu(readl(mem + UBOOT_BOOTCOUNT_MAGIC_OFFSET));
	if (magic != UBOOT_BOOTCOUNT_MAGIC) {
		dev_err(&ofdev->dev, "bad magic.\n");
		goto no_magic;
	}

	if (misc_register(&bootcount_miscdev)) {
		dev_err(&ofdev->dev, "failed to register device\n");
		goto misc_register_fail;
	}

	if (device_create_file(bootcount_miscdev.this_device,
		&dev_attr_bootcount)) {
		dev_warn(&ofdev->dev, "couldnt register sysFS entry.\n");
		goto register_sysfs_fail;
	}

	return 0;
register_sysfs_fail:
	misc_deregister(&bootcount_miscdev);
misc_register_fail:
no_magic:
	iounmap(mem);
	return -ENODEV;
}

static int bootcount_remove(struct platform_device *ofdev)
{
	misc_deregister(&bootcount_miscdev);
	iounmap(mem);
	return 0;
}

static const struct of_device_id bootcount_match[] = {
	{
		.compatible = "uboot,bootcount",
	},
	{},
};
MODULE_DEVICE_TABLE(of, bootcount_match);

static struct platform_driver bootcount_driver = {
	.driver = {
		.name = UBOOT_BOOTCOUNT_NAME,
		.of_match_table = bootcount_match,
		.owner = THIS_MODULE,
	},
	.probe = bootcount_probe,
	.remove = bootcount_remove,
};

static int __init uboot_bootcount_init(void)
{
	return platform_driver_register(&bootcount_driver);
}

static void __exit uboot_bootcount_cleanup(void)
{
	if (mem != NULL)
		iounmap(mem);
}

module_init(uboot_bootcount_init);
module_exit(uboot_bootcount_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steffen Rumler <steffen.rumler@siemens.com>");
MODULE_DESCRIPTION("Provide (read/write) access to the U-Boot bootcounter via sysFS");
