// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Leica Geosystems AG <https://leica-geosystems.com>
/*
 * HID driver for Leica KDU/KU devices with led and backlight support
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/slab.h>
#include "hid-hgs.h"

static int hgs_led_brightness_set_blocking(struct led_classdev *cdev,
					   enum led_brightness brightness)
{
	struct hgs_led *led = container_of(cdev, struct hgs_led, cdev);
	struct hgs_ctx *ctx = hid_get_drvdata(led->hdev);
	struct hid_field *field = led->field;

	if (!field)
		return -EINVAL;

	if (led->suspended) {
		cdev->brightness = LED_OFF;
		dev_err(&ctx->hid->dev, "LED is suspended, ignoring brightness change\n");
		return -EAGAIN;
	}

	brightness = clamp((int)brightness, field->logical_minimum, field->logical_maximum);

	mutex_lock(&ctx->lock);
	cdev->brightness = brightness;
	if (field->report_count >= led->dt.usage_index)
		field->value[led->dt.usage_index] = brightness;
	else
		field->value[0] = brightness;
	hid_hw_request(led->hdev, field->report, HID_REQ_SET_REPORT);
	fsleep(300);
	mutex_unlock(&ctx->lock);
	dev_dbg(&ctx->hid->dev, "LED brightness set to %u\n", brightness);
	return 0;
}

static int hgs_led_register(struct hgs_ctx *ctx, struct hgs_led *led)
{
	struct led_init_data init_data = {};

	init_data.fwnode = led->dt.fwnode;

	return devm_led_classdev_register_ext(&ctx->hid->dev, &led->cdev, &init_data);
}

static int hgs_register_leds_from_field(struct hgs_ctx *ctx, struct hid_field *field,
					struct hgs_led_dt *led_dt_array, int led_count)
{
	struct hgs_led *led;
	int i;
	int u;

	for (u = 0; u < field->maxusage; u++) {
		struct hid_usage usage = field->usage[u];

		for (i = 0; i < led_count; i++) {
			if (usage.hid == led_dt_array[i].reg &&
			    usage.usage_index == led_dt_array[i].usage_index) {
				led = devm_kzalloc(&ctx->hid->dev, sizeof(*led), GFP_KERNEL);
				if (!led)
					return -ENOMEM;

				INIT_LIST_HEAD(&led->list);
				led->cdev.brightness = field->logical_minimum;
				led->cdev.brightness_set_blocking = hgs_led_brightness_set_blocking;
				led->cdev.max_brightness = field->logical_maximum;
				led->dt = led_dt_array[i];
				led->field = field;
				led->hdev = ctx->hid;
				led->suspended = false;
				led->cdev.flags = LED_CORE_SUSPENDRESUME;
				if (hgs_led_register(ctx, led) < 0)
					dev_err(&ctx->hid->dev, "Failed to register LED\n");
				else
					list_add_tail(&led->list, &ctx->led_list);
			}
		}
	}

	return 0;
}

static int parse_led_nodes_from_dt(struct hgs_ctx *ctx, struct device_node *np_leds,
				   struct hgs_led_dt **led_dt_out)
{
	struct hgs_led_dt *led_dt;
	struct device_node *np;
	int valid_led = 0;
	int dt_led_count;
	int regs[2];
	int ret;

	dt_led_count = of_get_child_count(np_leds);
	if (dt_led_count <= 0) {
		dev_info(&ctx->hid->dev, "No led nodes found in device tree\n");
		return 0;
	}
	led_dt = devm_kzalloc(&ctx->hid->dev, dt_led_count * sizeof(*led_dt), GFP_KERNEL);
	if (!led_dt)
		return -ENOMEM;

	for_each_available_child_of_node(np_leds, np) {
		ret = of_property_read_u32_array(np, "reg", regs, 2);
		if (ret) {
			dev_err(&ctx->hid->dev, "Missing or invalid 'reg' property\n");
			of_node_put(np);
			continue;
		}

		led_dt[valid_led].reg = regs[0];
		led_dt[valid_led].usage_index = regs[1];
		led_dt[valid_led].fwnode = of_fwnode_handle(np);
		valid_led++;
		of_node_put(np);
	}

	*led_dt_out = led_dt;
	return valid_led;
}

static int hgs_register_leds_from_reports(struct hgs_ctx *ctx)
{
	struct hid_report_enum *re = &ctx->hid->report_enum[HID_OUTPUT_REPORT];
	struct device_node *of_node = dev_of_node(ctx->hid->dev.parent);
	struct device_node *np_leds;
	struct hid_report *report;
	struct hgs_led_dt *led_dt;
	int led_count;
	int ret = 0;

	if (!of_node)
		return -ENODEV;

	np_leds = of_get_child_by_name(of_node, LED_NODE_NAME);
	if (!np_leds) {
		dev_info(&ctx->hid->dev, "No LEDs found in device tree\n");
		return 0;
	}

	led_count = parse_led_nodes_from_dt(ctx, np_leds, &led_dt);
	if (led_count <= 0) {
		ret = led_count;
		goto cleanup;
	}

	list_for_each_entry(report, &re->report_list, list) {
		int idx;

		for (idx = 0; idx < report->maxfield; idx++) {
			struct hid_field *field = report->field[idx];
			unsigned int page = field->usage->hid >> 16;

			if (page != USAGE_PAGE_CONSUMER && page != USAGE_PAGE_KU)
				continue;

			ret = hgs_register_leds_from_field(ctx, field, led_dt, led_count);
			if (ret < 0)
				dev_err(&ctx->hid->dev, "Failed to register led: %d\n", ret);
		}
	}

cleanup:
	of_node_put(np_leds);
	return ret;
}

static int hgs_backlight_update_status(struct backlight_device *bl_dev)
{
	struct hgs_ctx *ctx = dev_get_drvdata(&bl_dev->dev);

	if (!ctx) {
		dev_err(&bl_dev->dev, "update_status: ctx is NULL\n");
		return -ENODEV;
	}

	if (ctx->backlight.suspended) {
		bl_dev->props.brightness = 0;
		return -EAGAIN;
	}

	struct hid_field *field = ctx->backlight.field;
	int brightness;

	if (!field) {
		dev_err(&ctx->hid->dev, "update_status: field is NULL\n");
		return -ENODEV;
	}

	brightness = bl_dev->props.brightness;
	brightness = clamp(brightness, field->logical_minimum, field->logical_maximum);
	if (backlight_is_blank(bl_dev))
		brightness = 0;

	mutex_lock(&ctx->lock);
	field->value[0] = brightness;
	hid_hw_request(ctx->hid, field->report, HID_REQ_SET_REPORT);
	fsleep(300);
	mutex_unlock(&ctx->lock);
	dev_dbg(&ctx->hid->dev, "Backlight brightness set to %d\n", brightness);
	return 0;
}

static const struct backlight_ops hgs_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = hgs_backlight_update_status,
};

static int hgs_backlight_register(struct hgs_ctx *ctx, struct hid_field *field, const char *name)
{
	struct device *parent_dev = ctx->hid->dev.parent;
	struct backlight_properties props;
	int ret;

	memset(&props, 0, sizeof(props));
	props.max_brightness = field->logical_maximum;
	props.brightness = field->logical_maximum;
	props.type = BACKLIGHT_RAW;
	ctx->backlight.field = field;
	ctx->backlight.suspended = false;
	ctx->backlight.dev = devm_backlight_device_register(&ctx->hid->dev, name, parent_dev, ctx,
							    &hgs_backlight_ops, &props);

	if (IS_ERR(ctx->backlight.dev)) {
		ret = PTR_ERR(ctx->backlight.dev);
		dev_err(&ctx->hid->dev, "Failed to register backlight %s: %d\n", name, ret);
		return ret;
	}

	/* Do not push brightness to hardware here: preserve what the bootloader
	 * programmed into the MCU. devm_backlight_device_register() does not
	 * write to hardware; suspend/resume call update_status() explicitly. */
	return 0;
}

static int hgs_register_backlight_from_reports(struct hgs_ctx *ctx)
{
	struct hid_report_enum *re = &ctx->hid->report_enum[HID_OUTPUT_REPORT];
	struct device_node *of_node, *np_backlight;
	struct hid_report *report;
	u32 regs[2];
	char *name;
	int ret;
	int u;
	int i;

	of_node = dev_of_node(ctx->hid->dev.parent);
	if (!of_node)
		return -ENODEV;

	np_backlight = of_get_child_by_name(of_node, BACKLIGHT_NODE_NAME);
	of_node_put(of_node);
	if (!np_backlight) {
		dev_err(&ctx->hid->dev, "backlight node not found\n");
		return -ENODEV;
	}

	ret = of_property_read_u32_array(np_backlight, "reg", regs, 2);
	if (ret) {
		dev_err(&ctx->hid->dev, "Missing or invalid 'reg' property\n");
		of_node_put(np_backlight);
		return ret;
	}

	ctx->backlight.resume_delay_us = 0;
	of_property_read_u32(np_backlight, "resume-delay-us", &ctx->backlight.resume_delay_us);
	of_node_put(np_backlight);
	name = devm_kasprintf(&ctx->hid->dev, GFP_KERNEL, "backlight-%s",
			      dev_name(ctx->hid->dev.parent));

	list_for_each_entry(report, &re->report_list, list) {
		for (i = 0; i < report->maxfield; i++) {
			struct hid_field *field = report->field[i];

			for (u = 0; u < field->maxusage; u++) {
				if (regs[0] == field->usage[u].hid &&
				    regs[1] == field->usage[u].usage_index) {
					return hgs_backlight_register(ctx, field, name);
				}
			}
		}
	}
	return ret;
}

static void hgs_cleanup_action(void *data)
{
	struct hgs_ctx *ctx = data;

	if (!ctx)
		return;

	hgs_fw_exit(ctx);
	hid_hw_stop(ctx->hid);
}

static void hgs_parse_fw_feature_field(struct hgs_ctx *ctx, struct hid_report *report,
				       struct hid_field *field)
{
	struct device *dev = &ctx->hid->dev;

	u32 usage_hid = field->usage[0].hid;

	if (report->type != HID_FEATURE_REPORT)
		return;

	if (usage_hid == HGS_USAGE_FW_VERSION_MAJOR || usage_hid == HGS_USAGE_FW_VERSION_MINOR ||
	    usage_hid == HGS_USAGE_FW_VERSION_BUILD) {
		if (!ctx->fw.version_feature_report) {
			ctx->fw.version_feature_report = report;
			dev_info(dev,
				 "Found FW Version usage (Usage 0x%X) in Feature Report ID %d\n",
				 usage_hid, report->id);
		}
		return;
	}

	if (usage_hid == HGS_USAGE_FW_MATERIAL &&
	    field->application == HGS_APPLICATION_FW_MATERIAL) {
		if (!ctx->fw.material_info_feature_report) {
			ctx->fw.material_info_feature_report = report;
			dev_info(dev, "Found Material Info (Usage 0x%X) in Feature Report ID %d\n",
				 usage_hid, report->id);
		}
		return;
	}

	if (usage_hid == HGS_USAGE_BOARD_INFO &&
	    field->application == HGS_APPLICATION_FW_BOARD_ID) {
		if (!ctx->fw.board_info_feature_report) {
			ctx->fw.board_info_feature_report = report;
			dev_info(dev, "Found Board Info (Usage 0x%X) in Feature Report ID %d\n",
				 usage_hid, report->id);
		}
		return;
	}

	if (usage_hid == HGS_USAGE_TRACE_ID && field->application == HGS_APPLICATION_FW_TRACE_ID) {
		if (!ctx->fw.trace_id_feature_report) {
			ctx->fw.trace_id_feature_report = report;
			dev_info(dev, "Found Trace ID (Usage 0x%X) in Feature Report ID %d\n",
				 usage_hid, report->id);
		}
		return;
	}
}

static void hgs_parse_fw_output_field(struct hgs_ctx *ctx, struct hid_report *report,
				      struct hid_field *field)
{
	struct device *dev = &ctx->hid->dev;
	u32 usage_hid = field->usage[0].hid;

	if (report->type != HID_OUTPUT_REPORT)
		return;

	if (usage_hid == HGS_USAGE_FW_STREAM_SIZE || usage_hid == HGS_USAGE_FW_STREAM_SIZE_LEGACY) {
		ctx->fw.output_report = report;
		ctx->fw.stream_size_field = field;
		dev_info(dev, "Found FW Stream Size (Usage 0x%X) in Output Report ID %d\n",
			 usage_hid, report->id);
		return;
	}

	if (usage_hid == HGS_USAGE_FW_DATA_BLOCK || usage_hid == HGS_USAGE_FW_DATA_BLOCK_LEGACY) {
		ctx->fw.output_report = report;
		ctx->fw.data_block_field = field;
		dev_info(dev, "Found FW Data Block (Usage 0x%X) in Output Report ID %d\n",
			 usage_hid, report->id);
		return;
	}
}

static void hgs_parse_fw_input_field(struct hgs_ctx *ctx, struct hid_report *report,
				     struct hid_field *field)
{
	struct device *dev = &ctx->hid->dev;

	u32 usage_hid = field->usage[0].hid;

	if (report->type != HID_INPUT_REPORT)
		return;

	if (usage_hid == HGS_USAGE_FW_STATE || usage_hid == HGS_USAGE_FW_STATE_LEGACY) {
		ctx->fw.input_report = report;
		ctx->fw.state_field = field;
		dev_info(dev, "Found FW State (Usage 0x%X) in Input Report ID %d\n", usage_hid,
			 report->id);
		return;
	}
}

static void hgs_hid_parse_update_reports(struct hgs_ctx *ctx)
{
	struct hid_device *hdev = ctx->hid;
	struct hid_report_enum *report_enum_array[] = {
		&hdev->report_enum[HID_OUTPUT_REPORT],
		&hdev->report_enum[HID_INPUT_REPORT],
		&hdev->report_enum[HID_FEATURE_REPORT]
	};

	struct hid_report *report;
	struct hid_field *field;
	int i, j;

	ctx->fw.output_report = NULL;
	ctx->fw.input_report = NULL;
	ctx->fw.version_feature_report = NULL;
	ctx->fw.stream_size_field = NULL;
	ctx->fw.data_block_field = NULL;
	ctx->fw.state_field = NULL;

	for (i = 0; i < ARRAY_SIZE(report_enum_array); i++) {
		list_for_each_entry(report, &report_enum_array[i]->report_list, list) {
			for (j = 0; j < report->maxfield; j++) {
				field = report->field[j];
				if (report->type == HID_OUTPUT_REPORT)
					hgs_parse_fw_output_field(ctx, report, field);
				else if (report->type == HID_INPUT_REPORT)
					hgs_parse_fw_input_field(ctx, report, field);
				else if (report->type == HID_FEATURE_REPORT)
					hgs_parse_fw_feature_field(ctx, report, field);
			}
		}
	}

	hgs_fw_init(ctx);
}

static int hgs_hid_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *raw_data,
			     int size)
{
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_fw_data *fw;
	unsigned long flags;

	if (!ctx)
		return 1;

	fw = &ctx->fw;

	if (report && fw->input_report && report->id == fw->input_report->id &&
	    report->type == HID_INPUT_REPORT) {
		if (size >= 2 && raw_data[0] == report->id) {
			u8 state_value = raw_data[1];

			spin_lock_irqsave(&fw->state_lock, flags);
			fw->last_fw_state = state_value;
			fw->fw_state_updated = true;
			spin_unlock_irqrestore(&fw->state_lock, flags);

			complete(&fw->fw_state_received);
		} else {
			dev_warn(&hdev->dev,
				 "Event: Report ID %d, unexpected size %d or raw_data[0] 0x%02x\n",
				 report->id, size, (size > 0 ? raw_data[0] : 0xFF));
		}
	}
	return 0;
}

static int hgs_hid_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hgs_ctx *ctx;
	int ret;

	ctx = devm_kzalloc(&hdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->hid = hdev;
	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->led_list);

	hid_set_drvdata(hdev, ctx);

	ret = hid_parse(hdev);
	if (ret) {
		dev_err(&hdev->dev, "parse failed: %d\n", ret);
		return ret;
	}

	ret = hgs_register_backlight_from_reports(ctx);
	if (ret)
		dev_warn(&hdev->dev, "Failed to register backlight: %d\n", ret);

	ret = hgs_register_leds_from_reports(ctx);
	if (ret)
		dev_warn(&hdev->dev, "Failed to register LEDs: %d\n", ret);

	hgs_hid_parse_update_reports(ctx);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		dev_err(&hdev->dev, "hid_hw_start failed: %d\n", ret);
		return ret;
	}

	ret = devm_add_action_or_reset(&hdev->dev, hgs_cleanup_action, ctx);
	if (ret) {
		dev_err(&hdev->dev, "Failed to add cleanup action: %d\n", ret);
		return ret;
	}

	return 0;
}

static int hgs_hid_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_led *led;

	if (!ctx)
		return 0;

	if (ctx->backlight.dev) {
		ctx->backlight.dev->props.power = BACKLIGHT_POWER_OFF;
		hgs_backlight_update_status(ctx->backlight.dev);
		ctx->backlight.suspended = true;
	}

	list_for_each_entry(led, &ctx->led_list, list) {
		if (led->cdev.brightness_set_blocking) {
			led->saved_brightness = led->cdev.brightness;
			led->cdev.brightness_set_blocking(&led->cdev, 0);
			led->suspended = true;
		}
	}

	return 0;
}

static int hgs_hid_resume(struct hid_device *hdev)
{
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_led *led;

	if (!ctx)
		return 0;

	if (ctx->backlight.dev) {
		ctx->backlight.dev->props.power = BACKLIGHT_POWER_ON;
		ctx->backlight.suspended = false;
		if (ctx->backlight.resume_delay_us > 0)
			fsleep(ctx->backlight.resume_delay_us);
		hgs_backlight_update_status(ctx->backlight.dev);
	}

	list_for_each_entry(led, &ctx->led_list, list) {
		if (led->cdev.brightness_set_blocking) {
			led->suspended = false;
			led->cdev.brightness_set_blocking(&led->cdev, led->saved_brightness);
		}
	}

	return 0;
}

static const struct hid_device_id hgs_hid_devices[] = {
	{ HID_I2C_DEVICE(0x16bd, 0x8000) },
	{ HID_I2C_DEVICE(0x16bd, 0x0057) },
	{ HID_I2C_DEVICE(0x16bd, 0x0056) },
	{}
};
MODULE_DEVICE_TABLE(hid, hgs_hid_devices);

static struct hid_driver hgs_hid_driver = {
	.id_table = hgs_hid_devices,
	.name = "hgs-hid",
	.probe = hgs_hid_probe,
	.raw_event = hgs_hid_raw_event,
	.reset_resume = hgs_hid_resume,
	.suspend = hgs_hid_suspend,
};

module_hid_driver(hgs_hid_driver);

MODULE_DESCRIPTION("Leica KDU/KU HID driver");
MODULE_LICENSE("GPL");
