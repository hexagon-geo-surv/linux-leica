/*
 * Copyright (c) 2014 Steffen Trumtrar <s.trumtrar@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __SOC_SOCFPGA_GPV_H__
#define __SOC_SOCFPGA_GPV_H__

#ifdef CONFIG_ARCH_SOCFPGA

#include <linux/device.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#define GPV_FN_MOD_BM_ISS_WR		BIT(1)
#define GPV_FN_MOD_BM_ISS_RD		BIT(0)
#define GPV_AHB_CNTL_FORCE_INCR		BIT(1)
#define GPV_AHB_CNTL_DECERR_EN		BIT(0)
#define GPV_WR_TIDEMARK_MASK		0xf
#define GPV_FN_MOD_AHB_WR_INCR_OVERRIDE	BIT(1)
#define GPV_FN_MOD_AHB_RD_INCR_OVERRIDE	BIT(0)
#define GPV_FN_MOD_WR			BIT(1)
#define GPV_FN_MOD_RD			BIT(0)
#define GPV_FN_MOD_BYPASS_MERGE		BIT(0)
#define GPV_READ_QOS_MASK		0xf
#define GPV_WRITE_QOS_MASK		0xf

static inline struct platform_device *socfpga_gpv_device_by_phandle(
							struct device_node *np,
							const char *name)
{
	struct device_node *gpv_np;
	struct platform_device *pdev;

	gpv_np = of_parse_phandle(np, name, 0);
	if (!gpv_np)
		return ERR_PTR(-EINVAL);

	pdev = of_find_device_by_node(gpv_np);

	of_node_put(gpv_np);

	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);

	return pdev;
}

#else

static struct platform_device *socfpga_gpv_device_by_phandle(
							struct device_node *np,
							const char *name)
{
	return ERR_PTR(-ENOSYS);
}

#endif
#endif
