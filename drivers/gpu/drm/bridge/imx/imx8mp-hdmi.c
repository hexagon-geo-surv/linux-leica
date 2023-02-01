// SPDX-License-Identifier: GPL-2.0+

/*
 * Copyright (C) 2022 Pengutronix, Lucas Stach <kernel@pengutronix.de>
 */

#include <drm/bridge/dw_hdmi.h>
#include <drm/drm_modes.h>
#include <linux/clk.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

struct imx_hdmi {
	struct dw_hdmi_plat_data plat_data;
	struct dw_hdmi *dw_hdmi;
	struct clk *pixclk;
	struct clk *fdcc;
};

static enum drm_mode_status
imx8mp_hdmi_mode_valid(struct dw_hdmi *dw_hdmi, void *data,
		       const struct drm_display_info *info,
		       const struct drm_display_mode *mode)
{
	struct imx_hdmi *hdmi = (struct imx_hdmi *)data;

	if (mode->clock < 13500)
		return MODE_CLOCK_LOW;

	if (mode->clock > 297000)
		return MODE_CLOCK_HIGH;

	if (clk_round_rate(hdmi->pixclk, mode->clock * 1000) !=
	    mode->clock * 1000)
		return MODE_CLOCK_RANGE;

	/* We don't support double-clocked and Interlaced modes */
	if ((mode->flags & DRM_MODE_FLAG_DBLCLK) ||
	    (mode->flags & DRM_MODE_FLAG_INTERLACE))
		return MODE_BAD;

	return MODE_OK;
}

static int imx8mp_hdmi_phy_init(struct dw_hdmi *dw_hdmi, void *data,
				const struct drm_display_info *display,
				const struct drm_display_mode *mode)
{
	return 0;
}

static void imx8mp_hdmi_phy_disable(struct dw_hdmi *dw_hdmi, void *data)
{
}

static const struct dw_hdmi_phy_ops imx8mp_hdmi_phy_ops = {
	.init		= imx8mp_hdmi_phy_init,
	.disable	= imx8mp_hdmi_phy_disable,
	.read_hpd	= dw_hdmi_phy_read_hpd,
	.update_hpd	= dw_hdmi_phy_update_hpd,
	.setup_hpd	= dw_hdmi_phy_setup_hpd,
};

static int imx_dw_hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_hdmi_plat_data *plat_data;
	struct imx_hdmi *hdmi;
	int ret;

	hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	plat_data = &hdmi->plat_data;

	hdmi->pixclk = devm_clk_get(dev, "pix");
	if (IS_ERR(hdmi->pixclk))
		return dev_err_probe(dev, PTR_ERR(hdmi->pixclk),
				     "Unable to get pixel clock\n");

	hdmi->fdcc = devm_clk_get(dev, "fdcc");
	if (IS_ERR(hdmi->fdcc))
		return dev_err_probe(dev, PTR_ERR(hdmi->fdcc),
				     "Unable to get FDCC clock\n");

	ret = clk_prepare_enable(hdmi->fdcc);
	if (ret)
		return dev_err_probe(dev, ret, "Unable to enable FDCC clock\n");

	plat_data->mode_valid = imx8mp_hdmi_mode_valid;
	plat_data->phy_ops = &imx8mp_hdmi_phy_ops;
	plat_data->phy_name = "SAMSUNG HDMI TX PHY";
	plat_data->priv_data = hdmi;

	hdmi->dw_hdmi = dw_hdmi_probe(pdev, plat_data);
	if (IS_ERR(hdmi->dw_hdmi))
		return PTR_ERR(hdmi->dw_hdmi);

	/*
	 * Just release PHY core from reset, all other power management is done
	 * by the PHY driver.
	 */
	dw_hdmi_phy_gen1_reset(hdmi->dw_hdmi);

	platform_set_drvdata(pdev, hdmi);

	return 0;
}

static int imx_dw_hdmi_remove(struct platform_device *pdev)
{
	struct imx_hdmi *hdmi = platform_get_drvdata(pdev);

	dw_hdmi_remove(hdmi->dw_hdmi);

	clk_disable_unprepare(hdmi->fdcc);

	return 0;
}

static const struct of_device_id imx_dw_hdmi_of_table[] = {
	{ .compatible = "fsl,imx8mp-hdmi" },
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, imx_dw_hdmi_of_table);

static struct platform_driver im_dw_hdmi_platform_driver = {
	.probe		= imx_dw_hdmi_probe,
	.remove		= imx_dw_hdmi_remove,
	.driver		= {
		.name	= "imx-dw-hdmi",
		.of_match_table = imx_dw_hdmi_of_table,
	},
};

module_platform_driver(im_dw_hdmi_platform_driver);

MODULE_DESCRIPTION("i.MX8M HDMI encoder driver");
MODULE_LICENSE("GPL");
