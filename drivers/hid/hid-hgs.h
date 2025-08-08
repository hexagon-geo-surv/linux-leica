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

#define HGS_USAGE_PAGE_FIRMWARE_UPDATE   0xff05
#define HGS_USAGE_FW_STREAM_SIZE  (HGS_USAGE_PAGE_FIRMWARE_UPDATE << 16 | 0x0002)
#define HGS_USAGE_FW_DATA_BLOCK   (HGS_USAGE_PAGE_FIRMWARE_UPDATE << 16 | 0x0003)
#define HGS_USAGE_FW_STATE        (HGS_USAGE_PAGE_FIRMWARE_UPDATE << 16 | 0x0004)

#define HGS_USAGE_PAGE_FIRMWARE_UPDATE_LEGACY   0xff01
#define HGS_USAGE_FW_STREAM_SIZE_LEGACY  (HGS_USAGE_PAGE_FIRMWARE_UPDATE_LEGACY << 16 | 0x0002)
#define HGS_USAGE_FW_DATA_BLOCK_LEGACY   (HGS_USAGE_PAGE_FIRMWARE_UPDATE_LEGACY << 16 | 0x0003)
#define HGS_USAGE_FW_STATE_LEGACY        (HGS_USAGE_PAGE_FIRMWARE_UPDATE_LEGACY << 16 | 0x0004)

#define HGS_USAGE_PAGE_FIRMWARE_INFO 0xff02
#define HGS_APPLICATION_FW_MATERIAL (HGS_USAGE_PAGE_FIRMWARE_INFO << 16 | 0x0003)
#define HGS_APPLICATION_FW_BOARD_ID (HGS_USAGE_PAGE_FIRMWARE_INFO << 16 | 0x0004)
#define HGS_APPLICATION_FW_TRACE_ID (HGS_USAGE_PAGE_FIRMWARE_INFO << 16 | 0x0005)

#define HGS_USAGE_PAGE_FIRMWARE_VERSION 0xff03
#define HGS_USAGE_FW_VERSION_MAJOR  (HGS_USAGE_PAGE_FIRMWARE_VERSION << 16 | 0x0001)
#define HGS_USAGE_FW_VERSION_MINOR  (HGS_USAGE_PAGE_FIRMWARE_VERSION << 16 | 0x0002)
#define HGS_USAGE_FW_VERSION_BUILD  (HGS_USAGE_PAGE_FIRMWARE_VERSION << 16 | 0x0003)

#define HGS_USAGE_PAGE_FIRMWARE_MATERIAL 0xff04
#define HGS_USAGE_FW_MATERIAL (HGS_USAGE_PAGE_FIRMWARE_MATERIAL << 16 | 0x0001)
#define HGS_USAGE_FW_REVESION (HGS_USAGE_PAGE_FIRMWARE_MATERIAL << 16 | 0x0002)

#define HGS_USAGE_PAGE_BOARD_INFO 0xff04
#define HGS_USAGE_BOARD_INFO  (HGS_USAGE_PAGE_BOARD_INFO << 16 | 0x0001)
#define HGS_USAGE_PRODUCTION_DATA (HGS_USAGE_PAGE_BOARD_INFO << 16 | 0x0002)
#define HGS_USAGE_CONSECUTIVE_NUMBER (HGS_USAGE_PAGE_BOARD_INFO << 16 | 0x0003)

#define HGS_USAGE_PAGE_TRACE_ID  0xff04
#define HGS_USAGE_TRACE_ID       (HGS_USAGE_PAGE_TRACE_ID << 16 | 0x0001)
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

	struct hgs_fw_data {
		struct hid_report *output_report;
		struct hid_report *input_report;
		struct hid_report *version_feature_report;
		struct hid_report *material_info_feature_report;
		struct hid_report *board_info_feature_report;
		struct hid_report *trace_id_feature_report;

		struct hid_field *stream_size_field;
		struct hid_field *data_block_field;
		struct hid_field *state_field;

		bool is_stream_cmd_little_endian; // true for little-endian

		/* For interrupt-driven state reading */
		u8 last_fw_state;
		bool fw_state_updated; // To confirm new state was actually set
		struct completion fw_state_received;
		spinlock_t state_lock; // Protects last_fw_state and fw_state_updated

		/* For firmware_upload_register */
		struct fw_upload *fwl;
		bool cancel;
		struct mutex cancel_lock;
	} fw;
};

/*
 * hgs_fw_init - init update support for firmware
 * @ctx:	HID ctx
 *
 * Returns 0 on success or negative error code.
 */
int hgs_fw_init(struct hgs_ctx *ctx);

/*
 * hgs_fw_exit - exit update support for firmware
 * @ctx:	HID ctx
 *
 * Cleans up resources allocated during hgs_fw_init.
 */
void hgs_fw_exit(struct hgs_ctx *ctx);

#endif /* __HID_HGS_H */
