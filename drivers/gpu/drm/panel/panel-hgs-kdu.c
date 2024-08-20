// SPDX-License-Identifier: GPL-2.0+
/*
 * Hexagon KDU panel driver
 *
 * Based on: pnael-lvds.c - Generic LVDS panel driver
 *
 * Copyright (C) 2016 Laurent Pinchart
 * Copyright (C) 2016 Renesas Electronics Corporation
 * Copyright (C) 2024 Marco Felsch
 *
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include <drm/drm_crtc.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>

struct panel_kdu {
	struct drm_panel panel;
	struct device *dev;

	const char *label;
	unsigned int width;
	unsigned int height;
	struct drm_display_mode dmode;
	u32 bus_flags;
	unsigned int bus_format;

	struct regulator *kdu1_supply;
	struct regulator *kdu2_supply;

	enum drm_panel_orientation orientation;
};

static inline struct panel_kdu *to_panel_kdu(struct drm_panel *panel)
{
	return container_of(panel, struct panel_kdu, panel);
}

static void panel_regulator_disable(struct regulator *supply)
{
	if (supply)
		regulator_disable(supply);
}

static int panel_kdu_unprepare(struct drm_panel *panel)
{
	struct panel_kdu *kdu = to_panel_kdu(panel);

	panel_regulator_disable(kdu->kdu1_supply);
	panel_regulator_disable(kdu->kdu2_supply);

	return 0;
}

static int panel_regulator_enable(struct regulator *supply)
{
	if (supply) {
		int err;

		err = regulator_enable(supply);
		if (err)
			return err;
	}

	return 0;
}

static int panel_kdu_prepare(struct drm_panel *panel)
{
	struct panel_kdu *kdu = to_panel_kdu(panel);
	int err;

	err = panel_regulator_enable(kdu->kdu1_supply);
	if (err) {
		dev_err(kdu->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	err = panel_regulator_enable(kdu->kdu2_supply);
	if (err) {
		panel_regulator_disable(kdu->kdu1_supply);
		dev_err(kdu->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	return 0;
}

static int panel_kdu_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct panel_kdu *kdu = to_panel_kdu(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &kdu->dmode);
	if (!mode)
		return 0;

	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = kdu->dmode.width_mm;
	connector->display_info.height_mm = kdu->dmode.height_mm;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &kdu->bus_format, 1);
	connector->display_info.bus_flags = kdu->bus_flags;

	/*
	 * TODO: Remove once all drm drivers call
	 * drm_connector_set_orientation_from_panel()
	 */
	drm_connector_set_panel_orientation(connector, kdu->orientation);

	return 1;
}

static enum drm_panel_orientation panel_kdu_get_orientation(struct drm_panel *panel)
{
	struct panel_kdu *kdu = to_panel_kdu(panel);

	return kdu->orientation;
}

static const struct drm_panel_funcs panel_kdu_funcs = {
	.unprepare = panel_kdu_unprepare,
	.prepare = panel_kdu_prepare,
	.get_modes = panel_kdu_get_modes,
	.get_orientation = panel_kdu_get_orientation,
};

static int panel_kdu_parse_dt(struct panel_kdu *kdu)
{
	struct device_node *np = kdu->dev->of_node;
	int ret;

	ret = of_drm_get_panel_orientation(np, &kdu->orientation);
	if (ret < 0) {
		dev_err(kdu->dev, "%pOF: failed to get orientation %d\n", np, ret);
		return ret;
	}

	ret = of_get_drm_panel_display_mode(np, &kdu->dmode, &kdu->bus_flags);
	if (ret < 0) {
		dev_err(kdu->dev, "%pOF: problems parsing panel-timing (%d)\n",
			np, ret);
		return ret;
	}

	of_property_read_string(np, "label", &kdu->label);

	ret = drm_of_lvds_get_data_mapping(np);
	if (ret < 0) {
		dev_err(kdu->dev, "%pOF: invalid or missing %s DT property\n",
			np, "data-mapping");
		return ret;
	}

	kdu->bus_format = ret;

	kdu->bus_flags |= of_property_read_bool(np, "data-mirror") ?
			   DRM_BUS_FLAG_DATA_LSB_TO_MSB :
			   DRM_BUS_FLAG_DATA_MSB_TO_LSB;

	return 0;
}

static struct regulator *panel_kdu_get_supply(struct panel_kdu *kdu, const char *supply_name)
{
	struct regulator *supply;

	supply = devm_regulator_get_optional(kdu->dev, supply_name);
	if (IS_ERR(supply)) {
		int ret;

		ret = PTR_ERR(supply);
		if (ret != -ENODEV) {
			if (ret != -EPROBE_DEFER)
				dev_err(kdu->dev, "failed to request regulator: %d\n",
					ret);
			return supply;
		}

		return NULL;
	}

	return supply;
}

static int panel_kdu_probe(struct platform_device *pdev)
{
	struct panel_kdu *kdu;
	int ret;

	kdu = devm_kzalloc(&pdev->dev, sizeof(*kdu), GFP_KERNEL);
	if (!kdu)
		return -ENOMEM;

	kdu->dev = &pdev->dev;

	ret = panel_kdu_parse_dt(kdu);
	if (ret < 0)
		return ret;

	kdu->kdu1_supply = panel_kdu_get_supply(kdu, "kdu1");
	if (IS_ERR(kdu->kdu1_supply))
		return PTR_ERR(kdu->kdu1_supply);

	kdu->kdu2_supply = panel_kdu_get_supply(kdu, "kdu2");
	if (IS_ERR(kdu->kdu2_supply))
		return PTR_ERR(kdu->kdu2_supply);

	/* Register the panel. */
	drm_panel_init(&kdu->panel, kdu->dev, &panel_kdu_funcs,
		       DRM_MODE_CONNECTOR_LVDS);

	/* TODO: clarify backlight handling */
	ret = drm_panel_of_backlight(&kdu->panel);
	if (ret)
		return ret;

	drm_panel_add(&kdu->panel);

	dev_set_drvdata(kdu->dev, kdu);
	return 0;
}

static void panel_kdu_remove(struct platform_device *pdev)
{
	struct panel_kdu *kdu = platform_get_drvdata(pdev);

	drm_panel_remove(&kdu->panel);

	drm_panel_disable(&kdu->panel);
}

static const struct of_device_id panel_kdu_of_table[] = {
	{ .compatible = "hgs,panel-kdu", },
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, panel_kdu_of_table);

static struct platform_driver panel_kdu_driver = {
	.probe		= panel_kdu_probe,
	.remove_new	= panel_kdu_remove,
	.driver		= {
		.name	= KBUILD_MODNAME,
		.of_match_table = panel_kdu_of_table,
	},
};
module_platform_driver(panel_kdu_driver);

MODULE_AUTHOR("Marco Felsch <kernel@pengutronix.de>");
MODULE_DESCRIPTION("Hexagon KDU Panel Driver");
MODULE_LICENSE("GPL");
