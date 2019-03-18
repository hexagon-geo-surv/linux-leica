/*
 * drivers/staging/android/ion/ion_leica.c
 *
 * Copyright (C) 2016 Leica Geosystems AG
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include "ion.h"
#include "ion_priv.h"

#define LEICA_INV_VAL (-1)

static phys_addr_t leica_rmem_base = LEICA_INV_VAL;
static phys_addr_t leica_rmem_size = LEICA_INV_VAL;
static struct ion_device *leica_ion_dev;
static struct ion_heap *leica_ion_heap;

static int __init leica_rmem_setup(struct reserved_mem *rmem)
{
	/* This function is called by kernel very early, it's not
	 * possible to create ION device at this point. Just store
	 * base address and size of reserved memory.
	 */
	if (leica_rmem_base != LEICA_INV_VAL) {
		pr_err("ion_leica: Only single region is supported!\n");
		return -EINVAL;
	}

	leica_rmem_base = rmem->base;
	leica_rmem_size = rmem->size;

	return 0;
}

RESERVEDMEM_OF_DECLARE(leica_rmem, "leica,ion", leica_rmem_setup);

static int __init leica_ion_init(void)
{
	struct ion_platform_heap carv_heap_data = {
		.id	= ION_HEAP_TYPE_CARVEOUT,
		.type	= ION_HEAP_TYPE_CARVEOUT,
		.name	= "carveout",
		.base	= leica_rmem_base,
		.size	= leica_rmem_size,
	};

	if (leica_rmem_base == LEICA_INV_VAL ||
	    leica_rmem_size == LEICA_INV_VAL) {
		pr_err("ion_leica: config not set! (0x%x@0x%x)\n",
		       leica_rmem_size, leica_rmem_base);
		return -EINVAL;
	}

	printk("ion_leica: base: 0x%x, size: 0x%x\n",
		 leica_rmem_base, leica_rmem_size);

	leica_ion_dev = ion_device_create(NULL);
	if (IS_ERR_OR_NULL(leica_ion_dev)) {
		pr_err("ion_leica: could not create device!\n");
		return -ENOMEM;
	}

	leica_ion_heap = ion_heap_create(&carv_heap_data);
	if (IS_ERR_OR_NULL(leica_ion_heap)) {
		pr_err("ion_leica: could not create heap!\n");
		ion_device_destroy(leica_ion_dev);
		leica_ion_dev = NULL;
		return -ENOMEM;
	}

	ion_device_add_heap(leica_ion_dev, leica_ion_heap);

	return 0;
}

static void __exit leica_ion_exit(void)
{
	pr_debug("ion_leica: exit (%p, %p)\n", leica_ion_dev, leica_ion_heap);
	if (leica_ion_dev)
		ion_device_destroy(leica_ion_dev);
	if (leica_ion_heap)
		ion_heap_destroy(leica_ion_heap);
}

module_init(leica_ion_init);
module_exit(leica_ion_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Leica Geosystems AG");
