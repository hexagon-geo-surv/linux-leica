// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2021 NXP
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx7-iomuxc-gpr.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <dt-bindings/phy/phy-imx8-pcie.h>

#define IMX8MM_PCIE_PHY_CMN_REG061	0x184
#define  ANA_PLL_CLK_OUT_TO_EXT_IO_EN	BIT(0)
#define IMX8MM_PCIE_PHY_CMN_REG062	0x188
#define  ANA_PLL_CLK_OUT_TO_EXT_IO_SEL	BIT(3)
#define IMX8MM_PCIE_PHY_CMN_REG063	0x18C
#define  AUX_PLL_REFCLK_SEL_SYS_PLL	GENMASK(7, 6)
#define IMX8MM_PCIE_PHY_CMN_REG064	0x190
#define  ANA_AUX_RX_TX_SEL_TX		BIT(7)
#define  ANA_AUX_RX_TERM_GND_EN		BIT(3)
#define  ANA_AUX_TX_TERM		BIT(2)
#define IMX8MM_PCIE_PHY_CMN_REG065	0x194
#define  ANA_AUX_RX_TERM		(BIT(7) | BIT(4))
#define  ANA_AUX_TX_LVL			GENMASK(3, 0)
#define IMX8MM_PCIE_PHY_CMN_REG075	0x1D4
#define  ANA_PLL_DONE			0x3
#define PCIE_PHY_TRSV_REG5		0x414
#define PCIE_PHY_TRSV_REG6		0x418

#define IMX8MM_GPR_PCIE_REF_CLK_SEL	GENMASK(25, 24)
#define IMX8MM_GPR_PCIE_REF_CLK_PLL	FIELD_PREP(IMX8MM_GPR_PCIE_REF_CLK_SEL, 0x3)
#define IMX8MM_GPR_PCIE_REF_CLK_EXT	FIELD_PREP(IMX8MM_GPR_PCIE_REF_CLK_SEL, 0x2)
#define IMX8MM_GPR_PCIE_AUX_EN		BIT(19)
#define IMX8MM_GPR_PCIE_CMN_RST		BIT(18)
#define IMX8MM_GPR_PCIE_POWER_OFF	BIT(17)
#define IMX8MM_GPR_PCIE_SSC_EN		BIT(16)
#define IMX8MM_GPR_PCIE_AUX_EN_OVERRIDE	BIT(9)

#define IMX8MP_GPR_REG0			0x0
#define IMX8MP_GPR_CLK_MOD_EN		BIT(0)
#define IMX8MP_GPR_PHY_APB_RST		BIT(4)
#define IMX8MP_GPR_PHY_INIT_RST		BIT(5)
#define IMX8MP_GPR_REG1			0x4
#define IMX8MP_GPR_PM_EN_CORE_CLK	BIT(0)
#define IMX8MP_GPR_PLL_LOCK		BIT(13)
#define IMX8MP_GPR_REG2			0x8
#define IMX8MP_GPR_P_PLL_MASK		GENMASK(5, 0)
#define IMX8MP_GPR_M_PLL_MASK		GENMASK(15, 6)
#define IMX8MP_GPR_S_PLL_MASK		GENMASK(18, 16)
#define IMX8MP_GPR_P_PLL		(0xc << 0)
#define IMX8MP_GPR_M_PLL		(0x320 << 6)
#define IMX8MP_GPR_S_PLL		(0x4 << 16)
#define IMX8MP_GPR_REG3			0xc
#define IMX8MP_GPR_PLL_CKE		BIT(17)
#define IMX8MP_GPR_PLL_RST		BIT(31)

enum imx8_pcie_phy_type {
	IMX8MM,
	IMX8MP,
};

struct imx8_pcie_phy {
	void __iomem		*base;
	struct device		*dev;
	struct clk		*clk;
	struct phy		*phy;
	struct regmap		*hsio_blk_ctrl;
	struct regmap		*iomuxc_gpr;
	struct reset_control	*reset;
	struct reset_control	*perst;
	u32			refclk_pad_mode;
	u32			tx_deemph_gen1;
	u32			tx_deemph_gen2;
	bool			clkreq_unused;
	enum imx8_pcie_phy_type	variant;
};

static int imx8_pcie_phy_init(struct phy *phy)
{
	int ret;
	u32 val, pad_mode;
	struct imx8_pcie_phy *imx8_phy = phy_get_drvdata(phy);

	reset_control_assert(imx8_phy->reset);

	pad_mode = imx8_phy->refclk_pad_mode;
	switch (imx8_phy->variant) {
	case IMX8MM:
		/* Tune PHY de-emphasis setting to pass PCIe compliance. */
		if (imx8_phy->tx_deemph_gen1)
			writel(imx8_phy->tx_deemph_gen1,
			       imx8_phy->base + PCIE_PHY_TRSV_REG5);
		if (imx8_phy->tx_deemph_gen2)
			writel(imx8_phy->tx_deemph_gen2,
			       imx8_phy->base + PCIE_PHY_TRSV_REG6);
		break;
	case IMX8MP:
		reset_control_assert(imx8_phy->perst);
		/* Set P=12,M=800,S=4 and must set ICP=2'b01. */
		regmap_update_bits(imx8_phy->hsio_blk_ctrl, IMX8MP_GPR_REG2,
				   IMX8MP_GPR_P_PLL_MASK |
				   IMX8MP_GPR_M_PLL_MASK |
				   IMX8MP_GPR_S_PLL_MASK,
				   IMX8MP_GPR_P_PLL |
				   IMX8MP_GPR_M_PLL |
				   IMX8MP_GPR_S_PLL);
		/* wait greater than 1/F_FREF =1/2MHZ=0.5us */
		udelay(1);

		regmap_update_bits(imx8_phy->hsio_blk_ctrl, IMX8MP_GPR_REG3,
				   IMX8MP_GPR_PLL_RST,
				   IMX8MP_GPR_PLL_RST);
		udelay(10);

		/* Set 1 to pll_cke of GPR_REG3 */
		regmap_update_bits(imx8_phy->hsio_blk_ctrl, IMX8MP_GPR_REG3,
				   IMX8MP_GPR_PLL_CKE,
				   IMX8MP_GPR_PLL_CKE);

		/* Lock time should be greater than 300cycle=300*0.5us=150us */
		ret = regmap_read_poll_timeout(imx8_phy->hsio_blk_ctrl,
					     IMX8MP_GPR_REG1, val,
					     val & IMX8MP_GPR_PLL_LOCK,
					     10, 1000);
		if (ret) {
			dev_err(imx8_phy->dev, "PCIe PLL lock timeout\n");
			return ret;
		}

		/* pcie_clock_module_en */
		regmap_update_bits(imx8_phy->hsio_blk_ctrl, IMX8MP_GPR_REG0,
				   IMX8MP_GPR_CLK_MOD_EN,
				   IMX8MP_GPR_CLK_MOD_EN);
		udelay(10);

		reset_control_deassert(imx8_phy->reset);
		reset_control_deassert(imx8_phy->perst);

		/* release pcie_phy_apb_reset and pcie_phy_init_resetn */
		regmap_update_bits(imx8_phy->hsio_blk_ctrl, IMX8MP_GPR_REG0,
				   IMX8MP_GPR_PHY_APB_RST |
				   IMX8MP_GPR_PHY_INIT_RST,
				   IMX8MP_GPR_PHY_APB_RST |
				   IMX8MP_GPR_PHY_INIT_RST);
		break;
	}

	if (pad_mode == IMX8_PCIE_REFCLK_PAD_INPUT) {
		/* Configure the pad as input */
		val = readl(imx8_phy->base + IMX8MM_PCIE_PHY_CMN_REG061);
		writel(val & ~ANA_PLL_CLK_OUT_TO_EXT_IO_EN,
		       imx8_phy->base + IMX8MM_PCIE_PHY_CMN_REG061);
	} else if (pad_mode == IMX8_PCIE_REFCLK_PAD_OUTPUT) {
		/* Configure the PHY to output the refclock via pad */
		writel(ANA_PLL_CLK_OUT_TO_EXT_IO_EN,
		       imx8_phy->base + IMX8MM_PCIE_PHY_CMN_REG061);
		writel(ANA_PLL_CLK_OUT_TO_EXT_IO_SEL,
		       imx8_phy->base + IMX8MM_PCIE_PHY_CMN_REG062);
		writel(AUX_PLL_REFCLK_SEL_SYS_PLL,
		       imx8_phy->base + IMX8MM_PCIE_PHY_CMN_REG063);
		val = ANA_AUX_RX_TX_SEL_TX | ANA_AUX_TX_TERM;
		writel(val | ANA_AUX_RX_TERM_GND_EN,
		       imx8_phy->base + IMX8MM_PCIE_PHY_CMN_REG064);
		writel(ANA_AUX_RX_TERM | ANA_AUX_TX_LVL,
		       imx8_phy->base + IMX8MM_PCIE_PHY_CMN_REG065);
	}

	/* Set AUX_EN_OVERRIDE 1'b0, when the CLKREQ# isn't hooked */
	regmap_update_bits(imx8_phy->iomuxc_gpr, IOMUXC_GPR14,
			   IMX8MM_GPR_PCIE_AUX_EN_OVERRIDE,
			   imx8_phy->clkreq_unused ?
			   0 : IMX8MM_GPR_PCIE_AUX_EN_OVERRIDE);
	regmap_update_bits(imx8_phy->iomuxc_gpr, IOMUXC_GPR14,
			   IMX8MM_GPR_PCIE_AUX_EN,
			   IMX8MM_GPR_PCIE_AUX_EN);
	regmap_update_bits(imx8_phy->iomuxc_gpr, IOMUXC_GPR14,
			   IMX8MM_GPR_PCIE_POWER_OFF, 0);
	regmap_update_bits(imx8_phy->iomuxc_gpr, IOMUXC_GPR14,
			   IMX8MM_GPR_PCIE_SSC_EN, 0);

	regmap_update_bits(imx8_phy->iomuxc_gpr, IOMUXC_GPR14,
			   IMX8MM_GPR_PCIE_REF_CLK_SEL,
			   pad_mode == IMX8_PCIE_REFCLK_PAD_INPUT ?
			   IMX8MM_GPR_PCIE_REF_CLK_EXT :
			   IMX8MM_GPR_PCIE_REF_CLK_PLL);
	usleep_range(100, 200);

	/* Do the PHY common block reset */
	regmap_update_bits(imx8_phy->iomuxc_gpr, IOMUXC_GPR14,
			   IMX8MM_GPR_PCIE_CMN_RST,
			   IMX8MM_GPR_PCIE_CMN_RST);

	switch (imx8_phy->variant) {
	case IMX8MM:
		reset_control_deassert(imx8_phy->reset);
		usleep_range(200, 500);
		break;

	case IMX8MP:
		/* wait for core_clk enabled */
		ret = regmap_read_poll_timeout(imx8_phy->hsio_blk_ctrl,
					     IMX8MP_GPR_REG1, val,
					     val & IMX8MP_GPR_PM_EN_CORE_CLK,
					     10, 20000);
		if (ret) {
			dev_err(imx8_phy->dev, "PCIe CORE CLK enable failed\n");
			return ret;
		}

		break;
	}

	/* Polling to check the phy is ready or not. */
	ret = readl_poll_timeout(imx8_phy->base + IMX8MM_PCIE_PHY_CMN_REG075,
				 val, val == ANA_PLL_DONE, 10, 20000);
	return ret;
}

static int imx8_pcie_phy_power_on(struct phy *phy)
{
	struct imx8_pcie_phy *imx8_phy = phy_get_drvdata(phy);

	return clk_prepare_enable(imx8_phy->clk);
}

static int imx8_pcie_phy_power_off(struct phy *phy)
{
	struct imx8_pcie_phy *imx8_phy = phy_get_drvdata(phy);

	clk_disable_unprepare(imx8_phy->clk);

	return 0;
}

static const struct phy_ops imx8_pcie_phy_ops = {
	.init		= imx8_pcie_phy_init,
	.power_on	= imx8_pcie_phy_power_on,
	.power_off	= imx8_pcie_phy_power_off,
	.owner		= THIS_MODULE,
};

static const struct of_device_id imx8_pcie_phy_of_match[] = {
	{.compatible = "fsl,imx8mm-pcie-phy", .data = (void *)IMX8MM},
	{.compatible = "fsl,imx8mp-pcie-phy", .data = (void *)IMX8MP},
	{ },
};
MODULE_DEVICE_TABLE(of, imx8_pcie_phy_of_match);

static int imx8_pcie_phy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	const struct of_device_id *of_id;
	struct device_node *np = dev->of_node;
	struct imx8_pcie_phy *imx8_phy;
	struct resource *res;

	of_id = of_match_device(imx8_pcie_phy_of_match, dev);
	if (!of_id)
		return -EINVAL;

	imx8_phy = devm_kzalloc(dev, sizeof(*imx8_phy), GFP_KERNEL);
	if (!imx8_phy)
		return -ENOMEM;

	imx8_phy->dev = dev;
	imx8_phy->variant = (enum imx8_pcie_phy_type)of_id->data;

	/* get PHY refclk pad mode */
	of_property_read_u32(np, "fsl,refclk-pad-mode",
			     &imx8_phy->refclk_pad_mode);

	if (of_property_read_u32(np, "fsl,tx-deemph-gen1",
				 &imx8_phy->tx_deemph_gen1))
		imx8_phy->tx_deemph_gen1 = 0;

	if (of_property_read_u32(np, "fsl,tx-deemph-gen2",
				 &imx8_phy->tx_deemph_gen2))
		imx8_phy->tx_deemph_gen2 = 0;

	if (of_property_read_bool(np, "fsl,clkreq-unsupported"))
		imx8_phy->clkreq_unused = true;
	else
		imx8_phy->clkreq_unused = false;

	imx8_phy->clk = devm_clk_get(dev, "ref");
	if (IS_ERR(imx8_phy->clk)) {
		dev_err(dev, "failed to get imx pcie phy clock\n");
		return PTR_ERR(imx8_phy->clk);
	}

	/* Grab GPR config register range */
	imx8_phy->iomuxc_gpr =
		 syscon_regmap_lookup_by_compatible("fsl,imx6q-iomuxc-gpr");
	if (IS_ERR(imx8_phy->iomuxc_gpr)) {
		dev_err(dev, "unable to find iomuxc registers\n");
		return PTR_ERR(imx8_phy->iomuxc_gpr);
	}

	imx8_phy->reset = devm_reset_control_get_exclusive(dev, "pciephy");
	if (IS_ERR(imx8_phy->reset)) {
		dev_err(dev, "Failed to get PCIEPHY reset control\n");
		return PTR_ERR(imx8_phy->reset);
	}
	if (imx8_phy->variant == IMX8MP) {
		/* Grab HSIO MIX config register range */
		imx8_phy->hsio_blk_ctrl =
			 syscon_regmap_lookup_by_compatible("fsl,imx8mp-hsio-blk-ctrl");
		if (IS_ERR(imx8_phy->hsio_blk_ctrl)) {
			dev_err(dev, "unable to find hsio mix registers\n");
			return PTR_ERR(imx8_phy->hsio_blk_ctrl);
		}

		imx8_phy->perst =
			devm_reset_control_get_exclusive(dev, "perst");
		if (IS_ERR(imx8_phy->perst)) {
			dev_err(dev, "Failed to get PCIEPHY perst control\n");
			return PTR_ERR(imx8_phy->perst);
		}
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	imx8_phy->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(imx8_phy->base))
		return PTR_ERR(imx8_phy->base);

	imx8_phy->phy = devm_phy_create(dev, NULL, &imx8_pcie_phy_ops);
	if (IS_ERR(imx8_phy->phy))
		return PTR_ERR(imx8_phy->phy);

	phy_set_drvdata(imx8_phy->phy, imx8_phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static struct platform_driver imx8_pcie_phy_driver = {
	.probe	= imx8_pcie_phy_probe,
	.driver = {
		.name	= "imx8-pcie-phy",
		.of_match_table	= imx8_pcie_phy_of_match,
	}
};
module_platform_driver(imx8_pcie_phy_driver);

MODULE_DESCRIPTION("FSL IMX8 PCIE PHY driver");
MODULE_LICENSE("GPL v2");
