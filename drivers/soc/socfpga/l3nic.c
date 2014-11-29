/*
 * Copyright 2014 Steffen Trumtrar <s.trumtrar@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <soc/socfpga/gpv.h>
#include <soc/socfpga/l3regs.h>

static const struct regmap_range l3nic_write_regs_range[] = {
	regmap_reg_range(L3NIC_REMAP, L3NIC_REMAP),
	regmap_reg_range(L3NIC_L4MAIN, L3NIC_LWHPS2FPGAREGS),
	regmap_reg_range(L3NIC_USB1, L3NIC_NANDDATA),
	regmap_reg_range(L3NIC_USB0, L3NIC_SDRDATA),
	regmap_reg_range(L3NIC_L4_MAIN_FN_MOD_BM_ISS, L3NIC_L4_MAIN_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_L4_SP_FN_MOD_BM_ISS, L3NIC_L4_SP_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_L4_MP_FN_MOD_BM_ISS, L3NIC_L4_MP_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_L4_OSC1_FN_MOD_BM_ISS, L3NIC_L4_OSC1_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_L4_SPIM_FN_MOD_BM_ISS, L3NIC_L4_SPIM_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_STM_FN_MOD_BM_ISS, L3NIC_STM_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_STM_FN_MOD, L3NIC_STM_FN_MOD),
	regmap_reg_range(L3NIC_LWHPS2FPGA_FN_MOD_BM_ISS, L3NIC_LWHPS2FPGA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_LWHPS2FPGA_FN_MOD, L3NIC_LWHPS2FPGA_FN_MOD),
	regmap_reg_range(L3NIC_USB1_FN_MOD_BM_ISS, L3NIC_USB1_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_USB1_AHB_CNTL, L3NIC_USB1_AHB_CNTL),
	regmap_reg_range(L3NIC_NANDDATA_FN_MOD_BM_ISS, L3NIC_NANDDATA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_NANDDATA_FN_MOD, L3NIC_NANDDATA_FN_MOD),
	regmap_reg_range(L3NIC_USB0_FN_MOD_BM_ISS, L3NIC_USB0_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_USB0_AHB_CNTL, L3NIC_USB0_AHB_CNTL),
	regmap_reg_range(L3NIC_QSPIDATA_FN_MOD_BM_ISS, L3NIC_QSPIDATA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_QSPIDATA_AHB_CNTL, L3NIC_QSPIDATA_AHB_CNTL),
	regmap_reg_range(L3NIC_FPGAMGRDATA_FN_MOD_BM_ISS, L3NIC_FPGAMGRDATA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_FPGAMGRDATA_WR_TIDEMARK, L3NIC_FPGAMGRDATA_WR_TIDEMARK),
	regmap_reg_range(L3NIC_FPGAMGRDATA_FN_MOD, L3NIC_FPGAMGRDATA_FN_MOD),
	regmap_reg_range(L3NIC_HPS2FPGA_FN_MOD_BM_ISS, L3NIC_HPS2FPGA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_HPS2FPGA_WR_TIDEMARK, L3NIC_HPS2FPGA_WR_TIDEMARK),
	regmap_reg_range(L3NIC_HPS2FPGA_FN_MOD, L3NIC_HPS2FPGA_FN_MOD),
	regmap_reg_range(L3NIC_ACP_FN_MOD_BM_ISS, L3NIC_ACP_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_ACP_FN_MOD, L3NIC_ACP_FN_MOD),
	regmap_reg_range(L3NIC_BOOT_ROM_FN_MOD_BM_ISS, L3NIC_BOOT_ROM_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_BOOT_ROM_FN_MOD, L3NIC_BOOT_ROM_FN_MOD),
	regmap_reg_range(L3NIC_OCRAM_FN_MOD_BM_ISS, L3NIC_OCRAM_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_OCRAM_WR_TIDEMARK, L3NIC_OCRAM_WR_TIDEMARK),
	regmap_reg_range(L3NIC_OCRAM_FN_MOD, L3NIC_OCRAM_FN_MOD),
	regmap_reg_range(L3NIC_DAP_FN_MOD2, L3NIC_DAP_FN_MOD_AHB),
	regmap_reg_range(L3NIC_DAP_READ_QOS, L3NIC_DAP_FN_MOD),
	regmap_reg_range(L3NIC_MPU_READ_QOS, L3NIC_MPU_FN_MOD),
	regmap_reg_range(L3NIC_SDMMC_FN_MOD_AHB, L3NIC_SDMMC_FN_MOD_AHB),
	regmap_reg_range(L3NIC_SDMMC_READ_QOS, L3NIC_SDMMC_FN_MOD),
	regmap_reg_range(L3NIC_DMA_READ_QOS, L3NIC_DMA_FN_MOD),
	regmap_reg_range(L3NIC_FPGA2HPS_WR_TIDEMARK, L3NIC_FPGA2HPS_WR_TIDEMARK),
	regmap_reg_range(L3NIC_FPGA2HPS_READ_QOS, L3NIC_FPGA2HPS_FN_MOD),
	regmap_reg_range(L3NIC_ETR_READ_QOS, L3NIC_ETR_FN_MOD),
	regmap_reg_range(L3NIC_EMAC0_READ_QOS, L3NIC_EMAC0_FN_MOD),
	regmap_reg_range(L3NIC_EMAC1_READ_QOS, L3NIC_EMAC1_FN_MOD),
	regmap_reg_range(L3NIC_USB0_FN_MOD_AHB, L3NIC_USB0_FN_MOD_AHB),
	regmap_reg_range(L3NIC_USB0_READ_QOS, L3NIC_USB0_FN_MOD),
	regmap_reg_range(L3NIC_NAND_READ_QOS, L3NIC_NAND_FN_MOD),
	regmap_reg_range(L3NIC_USB1_FN_MOD_AHB, L3NIC_USB1_FN_MOD_AHB),
	regmap_reg_range(L3NIC_USB1_READ_QOS, L3NIC_USB1_FN_MOD),
};

static const struct regmap_range l3nic_read_regs_range[] = {
	regmap_reg_range(L3NIC_REMAP, L3NIC_REMAP),

	regmap_reg_range(L3NIC_PERIPH_ID_4, L3NIC_PERIPH_ID_4),
	regmap_reg_range(L3NIC_PERIPH_ID_0, L3NIC_COMP_ID_3),

	regmap_reg_range(L3NIC_L4_MAIN_FN_MOD_BM_ISS, L3NIC_L4_MAIN_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_L4_SP_FN_MOD_BM_ISS, L3NIC_L4_SP_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_L4_MP_FN_MOD_BM_ISS, L3NIC_L4_MP_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_L4_OSC1_FN_MOD_BM_ISS, L3NIC_L4_OSC1_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_L4_SPIM_FN_MOD_BM_ISS, L3NIC_L4_SPIM_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_STM_FN_MOD_BM_ISS, L3NIC_STM_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_STM_FN_MOD, L3NIC_STM_FN_MOD),
	regmap_reg_range(L3NIC_LWHPS2FPGA_FN_MOD_BM_ISS, L3NIC_LWHPS2FPGA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_LWHPS2FPGA_FN_MOD, L3NIC_LWHPS2FPGA_FN_MOD),
	regmap_reg_range(L3NIC_USB1_FN_MOD_BM_ISS, L3NIC_USB1_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_USB1_AHB_CNTL, L3NIC_USB1_AHB_CNTL),
	regmap_reg_range(L3NIC_NANDDATA_FN_MOD_BM_ISS, L3NIC_NANDDATA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_NANDDATA_FN_MOD, L3NIC_NANDDATA_FN_MOD),
	regmap_reg_range(L3NIC_USB0_FN_MOD_BM_ISS, L3NIC_USB0_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_USB0_AHB_CNTL, L3NIC_USB0_AHB_CNTL),
	regmap_reg_range(L3NIC_QSPIDATA_FN_MOD_BM_ISS, L3NIC_QSPIDATA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_QSPIDATA_AHB_CNTL, L3NIC_QSPIDATA_AHB_CNTL),
	regmap_reg_range(L3NIC_FPGAMGRDATA_FN_MOD_BM_ISS, L3NIC_FPGAMGRDATA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_FPGAMGRDATA_WR_TIDEMARK, L3NIC_FPGAMGRDATA_WR_TIDEMARK),
	regmap_reg_range(L3NIC_FPGAMGRDATA_FN_MOD, L3NIC_FPGAMGRDATA_FN_MOD),
	regmap_reg_range(L3NIC_HPS2FPGA_FN_MOD_BM_ISS, L3NIC_HPS2FPGA_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_HPS2FPGA_WR_TIDEMARK, L3NIC_HPS2FPGA_WR_TIDEMARK),
	regmap_reg_range(L3NIC_HPS2FPGA_FN_MOD, L3NIC_HPS2FPGA_FN_MOD),
	regmap_reg_range(L3NIC_ACP_FN_MOD_BM_ISS, L3NIC_ACP_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_ACP_FN_MOD, L3NIC_ACP_FN_MOD),
	regmap_reg_range(L3NIC_BOOT_ROM_FN_MOD_BM_ISS, L3NIC_BOOT_ROM_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_BOOT_ROM_FN_MOD, L3NIC_BOOT_ROM_FN_MOD),
	regmap_reg_range(L3NIC_OCRAM_FN_MOD_BM_ISS, L3NIC_OCRAM_FN_MOD_BM_ISS),
	regmap_reg_range(L3NIC_OCRAM_WR_TIDEMARK, L3NIC_OCRAM_WR_TIDEMARK),
	regmap_reg_range(L3NIC_OCRAM_FN_MOD, L3NIC_OCRAM_FN_MOD),
	regmap_reg_range(L3NIC_DAP_FN_MOD2, L3NIC_DAP_FN_MOD_AHB),
	regmap_reg_range(L3NIC_DAP_READ_QOS, L3NIC_DAP_FN_MOD),
	regmap_reg_range(L3NIC_MPU_READ_QOS, L3NIC_MPU_FN_MOD),
	regmap_reg_range(L3NIC_SDMMC_FN_MOD_AHB, L3NIC_SDMMC_FN_MOD_AHB),
	regmap_reg_range(L3NIC_SDMMC_READ_QOS, L3NIC_SDMMC_FN_MOD),
	regmap_reg_range(L3NIC_DMA_READ_QOS, L3NIC_DMA_FN_MOD),
	regmap_reg_range(L3NIC_FPGA2HPS_WR_TIDEMARK, L3NIC_FPGA2HPS_WR_TIDEMARK),
	regmap_reg_range(L3NIC_FPGA2HPS_READ_QOS, L3NIC_FPGA2HPS_FN_MOD),
	regmap_reg_range(L3NIC_ETR_READ_QOS, L3NIC_ETR_FN_MOD),
	regmap_reg_range(L3NIC_EMAC0_READ_QOS, L3NIC_EMAC0_FN_MOD),
	regmap_reg_range(L3NIC_EMAC1_READ_QOS, L3NIC_EMAC1_FN_MOD),
	regmap_reg_range(L3NIC_USB0_FN_MOD_AHB, L3NIC_USB0_FN_MOD_AHB),
	regmap_reg_range(L3NIC_USB0_READ_QOS, L3NIC_USB0_FN_MOD),
	regmap_reg_range(L3NIC_NAND_READ_QOS, L3NIC_NAND_FN_MOD),
	regmap_reg_range(L3NIC_USB1_FN_MOD_AHB, L3NIC_USB1_FN_MOD_AHB),
	regmap_reg_range(L3NIC_USB1_READ_QOS, L3NIC_USB1_FN_MOD),
};

static const struct regmap_access_table l3nic_write_regs = {
	.yes_ranges = l3nic_write_regs_range,
	.n_yes_ranges = ARRAY_SIZE(l3nic_write_regs_range),
};

static const struct regmap_access_table l3nic_read_regs = {
	.yes_ranges = l3nic_read_regs_range,
	.n_yes_ranges = ARRAY_SIZE(l3nic_read_regs_range),
};

static struct regmap_config l3nic_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.rd_table = &l3nic_read_regs,
	.wr_table = &l3nic_write_regs,
	.cache_type = REGCACHE_RBTREE,
};

struct regmap *socfpga_l3nic_regmap_by_phandle(struct device_node *np,
					       const char *name)
{
	struct socfpga_l3nic *l3nic;
	struct platform_device *pdev;

	pdev = socfpga_gpv_device_by_phandle(np, name);
	if (IS_ERR(pdev))
		return ERR_CAST(pdev);

	l3nic = dev_get_drvdata(&pdev->dev);
	if (!l3nic)
		return ERR_PTR(-EINVAL);

	if (l3nic->regmap)
		return l3nic->regmap;
	else
		return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL_GPL(socfpga_l3nic_regmap_by_phandle);

static int socfpga_l3nic_probe(struct platform_device *pdev)
{
	struct socfpga_l3nic *priv;
	struct resource *res;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	l3nic_regmap_config.max_register = res->end - res->start - 3;
	priv->regmap = devm_regmap_init_mmio(&pdev->dev, priv->base,
					     &l3nic_regmap_config);
	if (IS_ERR(priv->regmap)) {
		dev_err(&pdev->dev, "regmap init failed\n");
		return PTR_ERR(priv->regmap);
	}

	platform_set_drvdata(pdev, priv);

	dev_info(&pdev->dev, "L3 NIC-301 registered\n");

	return 0;
}

static int socfpga_l3nic_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id socfpga_l3nic_dt_ids[] = {
	{ .compatible = "altr,l3-nic", },
	{ /* sentinel */ },
};

static struct platform_driver socfpga_l3nic_driver = {
	.probe	= socfpga_l3nic_probe,
	.remove	= socfpga_l3nic_remove,
	.driver = {
		.name		= "socfpga-l3-nic",
		.owner		= THIS_MODULE,
		.of_match_table	= socfpga_l3nic_dt_ids,
	},
};
module_platform_driver(socfpga_l3nic_driver);

MODULE_AUTHOR("Steffen Trumtrar <s.trumtrar@pengutronix.de");
MODULE_DESCRIPTION("Socfpga L3 NIC-301 Interconnect Driver");
MODULE_LICENSE("GPL v2");
