/* SPDX-License-Identifier: GPL-2.0-only */
// SPDX-FileCopyrightText: Leica Geosystems AG <https://leica-geosystems.com>

#ifndef __HID_HGS_H
#define __HID_HGS_H

#include <linux/backlight.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "hid-ids.h"

#define LED_NODE_NAME "leds"
#define BACKLIGHT_NODE_NAME "backlight"
#define USAGE_PAGE_CONSUMER 0x0c
#define USAGE_PAGE_KU 0xff10

struct hgs_led_dt {
	struct fwnode_handle *fwnode;
	int usage_index;
	int reg;
};

struct hgs_led {
	bool suspended;
	enum led_brightness saved_brightness;
	struct hgs_led_dt dt;
	struct hid_device *hdev;
	struct hid_field *field;
	struct led_classdev cdev;
	struct list_head list;
};

struct hgs_backlight {
	bool suspended;
	struct backlight_device *dev;
	struct hid_field *field;
	u32 resume_delay_us;
};

struct hgs_ctx {
	struct hgs_backlight backlight;
	struct hid_device *hid;
	struct list_head led_list;
	struct mutex lock;
};

#endif /* __HID_HGS_H */
