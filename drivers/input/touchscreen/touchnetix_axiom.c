// SPDX-License-Identifier: GPL-2.0-only
/*
 * TouchNetix aXiom Touchscreen Driver
 *
 * Copyright (C) 2024 Pengutronix
 *
 * Marco Felsch <kernel@pengutronix.de>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/crc16.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/time.h>
#include <linux/unaligned.h>

/*
 * Short introduction for developers:
 *  The programming manual is written based on u(sages):
 *   - Max. 0xff usages possible
 *   - A usage is a group of registers (0x00 ... 0xff)
 *   - The usage base address must be discovered (FW dependent)
 *   - Partial RW usage access is allowed
 *   - Each usage has a revision (FW dependent)
 *   - Only u31 is always at address 0x0 (used for discovery)
 *
 *  E.x. Reading register 0x01 for usage u03 with baseaddr 0x20 results in the
 *  following physical 16bit I2C address: 0x2001.
 *
 * Note the datasheet specifies the usage numbers in hex and the internal
 * offsets in decimal. Keep it that way to make it more developer friendly.
 */
#define AXIOM_U01				0x01
#define AXIOM_U01_REV1_REPORTTYPE_REG		0
#define   AXIOM_U01_REV1_REPORTTYPE_HELLO	0
#define   AXIOM_U01_REV1_REPORTTYPE_HEARTBEAT	1
#define   AXIOM_U01_REV1_REPORTTYPE_OPCOMPLETE	3

#define AXIOM_U02					0x02
#define AXIOM_U02_REV1_COMMAND_REG			0
#define   AXIOM_U02_REV1_CMD_HARDRESET			0x0001
#define   AXIOM_U02_REV1_CMD_SOFTRESET			0x0002
#define   AXIOM_U02_REV1_CMD_STOP			0x0005
#define   AXIOM_U02_REV1_CMD_SAVEVLTLCFG2NVM		0x0007
#define   AXIOM_U02_REV1_PARAM1_SAVEVLTLCFG2NVM		0xb10c
#define   AXIOM_U02_REV1_PARAM2_SAVEVLTLCFG2NVM		0xc0de
#define   AXIOM_U02_REV1_CMD_HANDSHAKENVM		0x0008
#define   AXIOM_U02_REV1_CMD_COMPUTECRCS		0x0009
#define   AXIOM_U02_REV1_CMD_FILLCONFIG			0x000a
#define   AXIOM_U02_REV1_PARAM0_FILLCONFIG		0x5555
#define   AXIOM_U02_REV1_PARAM1_FILLCONFIG		0xaaaa
#define   AXIOM_U02_REV1_PARAM2_FILLCONFIG_ZERO		0xa55a
#define   AXIOM_U02_REV1_CMD_ENTERBOOTLOADER		0x000b
#define   AXIOM_U02_REV1_PARAM0_ENTERBOOLOADER_KEY1	0x5555
#define   AXIOM_U02_REV1_PARAM0_ENTERBOOLOADER_KEY2	0xaaaa
#define   AXIOM_U02_REV1_PARAM0_ENTERBOOLOADER_KEY3	0xa55a
#define   AXIOM_U02_REV1_RESP_SUCCESS			0x0000

struct axiom_u02_rev1_system_manager_msg {
	union {
		__le16 command;
		__le16 response;
	};
	__le16 parameters[3];
};

#define AXIOM_U04				0x04
#define   AXIOM_U04_REV1_SIZE_BYTES		128

#define AXIOM_U05				0x05	/* CDU */

#define AXIOM_U22				0x22	/* CDU */

#define AXIOM_U31				0x31
#define AXIOM_U31_REV1_PAGE0			0x0000
#define AXIOM_U31_REV1_DEVICE_ID_LOW_REG	(AXIOM_U31_REV1_PAGE0 + 0)
#define AXIOM_U31_REV1_DEVICE_ID_HIGH_REG	(AXIOM_U31_REV1_PAGE0 + 1)
#define   AXIOM_U31_REV1_MODE_MASK		BIT(7)
#define   AXIOM_U31_REV1_MODE_BLP		1
#define   AXIOM_U31_REV1_DEVICE_ID_HIGH_MASK	GENMASK(6, 0)
#define AXIOM_U31_REV1_RUNTIME_FW_MIN_REG	(AXIOM_U31_REV1_PAGE0 + 2)
#define AXIOM_U31_REV1_RUNTIME_FW_MAJ_REG	(AXIOM_U31_REV1_PAGE0 + 3)
#define AXIOM_U31_REV1_RUNTIME_FW_STATUS_REG	(AXIOM_U31_REV1_PAGE0 + 4)
#define   AXIOM_U31_REV1_RUNTIME_FW_STATUS	BIT(7)
#define AXIOM_U31_REV1_JEDEC_ID_LOW_REG		(AXIOM_U31_REV1_PAGE0 + 8)
#define AXIOM_U31_REV1_JEDEC_ID_HIGH_REG	(AXIOM_U31_REV1_PAGE0 + 9)
#define AXIOM_U31_REV1_NUM_USAGES_REG		(AXIOM_U31_REV1_PAGE0 + 10)
#define AXIOM_U31_REV1_RUNTIME_FW_RC_REG	(AXIOM_U31_REV1_PAGE0 + 11)
#define   AXIOM_U31_REV1_RUNTIME_FW_RC_MASK	GENMASK(7, 4)
#define   AXIOM_U31_REV1_SILICON_REV_MASK	GENMASK(3, 0)

#define AXIOM_U31_REV1_PAGE1			0x0100
#define   AXIOM_U31_REV1_OFFSET_TYPE_MASK	BIT(7)
#define   AXIOM_U31_REV1_MAX_OFFSET_MASK	GENMASK(6, 0)

#define AXIOM_U32				0x32

struct axiom_u31_usage_table_entry {
	u8 usage_num;
	u8 start_page;
	u8 num_pages;
	u8 max_offset;
	u8 uifrevision;
	u8 reserved;
} __packed;

#define AXIOM_U33				0x33

struct axiom_u33_rev2 {
	__le32 runtime_crc;
	__le32 runtime_nvm_crc;
	__le32 bootloader_crc;
	__le32 nvltlusageconfig_crc;
	__le32 vltusageconfig_crc;
	__le32 u22_sequencedata_crc;
	__le32 u43_hotspots_crc;
	__le32 u93_profiles_crc;
	__le32 u94_deltascalemap_crc;
	__le32 runtimehash_crc;
};

#define AXIOM_U34				0x34
#define   AXIOM_U34_REV1_OVERFLOW_MASK		BIT(7)
#define   AXIOM_U34_REV1_REPORTLENGTH_MASK	GENMASK(6, 0)
#define   AXIOM_U34_REV1_PREAMBLE_BYTES		2
#define   AXIOM_U34_REV1_POSTAMBLE_BYTES	4

#define AXIOM_U36				0x36

#define AXIOM_U41				0x41
#define AXIOM_U41_REV2_TARGETSTATUS_REG		0
#define AXIOM_U41_REV2_X_REG(id)		((4 * (id)) + 2)
#define AXIOM_U41_REV2_Y_REG(id)		((4 * (id)) + 4)
#define AXIOM_U41_REV2_Z_REG(id)		((id) + 42)

#define AXIOM_U42				0x42
#define AXIOM_U42_REV1_REPORT_ID_CONTAINS(id)	((id) + 2)
#define   AXIOM_U42_REV1_REPORT_ID_TOUCH	1	/* Touch, Proximity, Hover */

#define AXIOM_U43				0x43	/* CDU */

#define AXIOM_U64					0x64
#define   AXIOM_U64_REV2_ENABLECDSPROCESSING_REG	0
#define   AXIOM_U64_REV2_ENABLECDSPROCESSING_MASK	BIT(0)

#define AXIOM_U77				0x77	/* CDU */
#define AXIOM_U82				0x82
#define AXIOM_U93				0x93	/* CDU */
#define AXIOM_U94				0x94	/* CDU */

/*
 * Axiom CDU usage structure copied from downstream CDU_Common.py. Downstream
 * doesn't mention any revision. According downstream all CDU register windows
 * are 56 byte wide (8 byte header + 48 byte data).
 */
#define AXIOM_CDU_CMD_STORE			0x0002
#define AXIOM_CDU_CMD_COMMIT			0x0003
#define AXIOM_CDU_PARAM0_COMMIT			0xb10c
#define AXIOM_CDU_PARAM1_COMMIT			0xc0de

#define AXIOM_CDU_RESP_SUCCESS			0x0000
#define AXIOM_CDU_MAX_DATA_BYTES		48

struct axiom_cdu_usage {
	union {
		__le16 command;
		__le16 response;
	};
	__le16 parameters[3];
	u8 data[AXIOM_CDU_MAX_DATA_BYTES];
};

/*
 * u01 for the bootloader protocol (BLP)
 *
 * Values taken from Bootloader.py [1] which had a comment that documentation
 * values are out dated. The BLP does not have different versions according the
 * documentation python helper.
 *
 * [1] https://github.com/TouchNetix/axiom_pylib
 */
#define AXIOM_U01_BLP_COMMAND_REG		0x0100
#define   AXIOM_U01_BLP_COMMAND_RESET		BIT(1)
#define AXIOM_U01_BLP_SATUS_REG			0x0100
#define   AXIOM_U01_BLP_STATUS_BUSY		BIT(0)
#define AXIOM_U01_BLP_FIFO_REG			0x0102
#define   AXIOM_U01_BLP_FIFO_CHK_SIZE_BYTES	255

#define AXIOM_PROX_LEVEL			-128
#define AXIOM_STARTUP_TIME_MS			110

#define AXIOM_USAGE_BASEADDR_MASK		GENMASK(15, 8)
#define AXIOM_MAX_USAGES			256	/* u00 - uFF */
/*
 * The devices have a 16bit ADC but Touchnetix used the lower two bits for other
 * information.
 */
#define AXIOM_MAX_XY				(65535 - 3)
#define AXIOM_DEFAULT_POLL_INTERVAL_MS		10
#define AXIOM_PAGE_BYTE_LEN			256
#define AXIOM_MAX_XFERLEN			0x7fff
#define AXIOM_MAX_TOUCHSLOTS			10
#define AXIOM_MAX_TOUCHSLOTS_MASK		GENMASK(9, 0)

/* aXiom firmware (.axfw) */
#define AXIOM_FW_AXFW_SIGNATURE			"AXFW"
#define AXIOM_FW_AXFW_FILE_FMT_VER		0x0200

struct axiom_fw_axfw_hdr {
	u8 signature[4];
	__le32 file_crc32;
	__le16 file_format_ver;
	__le16 device_id;
	u8 variant;
	u8 minor_ver;
	u8 major_ver;
	u8 rc_ver;
	u8 status;
	__le16 silicon_ver;
	u8 silicon_rev;
	__le32 fw_crc32;
} __packed;

struct axiom_fw_axfw_chunk_hdr {
	u8 internal[6]; /* no description */
	__be16 payload_length;
};

/* aXiom config (.th2cfgbin) */
#define AXIOM_FW_CFG_SIGNATURE			0x20071969

struct axiom_fw_cfg_hdr {
	__be32 signature;
	__le16 file_format_ver;
	__le16 tcp_file_rev_major;
	__le16 tcp_file_rev_minor;
	__le16 tcp_file_rev_patch;
	u8 tcp_version;
} __packed;

struct axiom_fw_cfg_chunk_hdr {
	u8 usage_num;
	u8 usage_rev;
	u8 reserved;
	__le16 usage_length;
} __packed;

struct axiom_fw_cfg_chunk {
	u8 usage_num;
	u8 usage_rev;
	u16 usage_length;
	const u8 *usage_content;
};

enum axiom_fw_type {
	AXIOM_FW_AXFW,
	AXIOM_FW_CFG,
	AXIOM_FW_NUM
};

enum axiom_crc_type {
	AXIOM_CRC_CUR,
	AXIOM_CRC_NEW,
	AXIOM_CRC_NUM
};

struct axiom_data;

struct axiom_usage_info {
	unsigned char usage_num;	/* uXX number (XX in hex) */
	unsigned char rev_num;		/* rev.X (X in dec) */
	bool is_cdu;
	bool is_ro;

	/* Optional hooks */
	int (*process_report)(struct axiom_data *ts, const u8 *buf, size_t bufsize);
};

enum axiom_runmode {
	AXIOM_DISCOVERY_MODE,
	AXIOM_TCP_MODE,
	AXIOM_TCP_CFG_UPDATE_MODE,
	AXIOM_BLP_PRE_MODE,
	AXIOM_BLP_MODE,
};

struct axiom_data {
	struct input_dev *input;
	struct device *dev;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[2];
	unsigned int num_supplies;

	struct regmap *regmap;
	struct touchscreen_properties prop;
	bool irq_setup_done;
	u32 poll_interval;

	enum axiom_runmode mode;
	/*
	 * Two completion types to support firmware updates
	 * in irq and poll mode.
	 */
	struct axiom_completion {
		struct completion completion;
		bool poll_done;
	} nvm_write, boot_complete;

	/* Lock to protect both firmware interfaces */
	struct mutex fwupdate_lock;
	struct axiom_firmware {
		/* Lock to protect cancel */
		struct mutex lock;
		bool cancel;
		struct fw_upload *fwl;
	} fw[AXIOM_FW_NUM];

	unsigned int fw_major;
	unsigned int fw_minor;
	unsigned int fw_rc;
	unsigned int fw_status;
	u16 device_id;
	u16 jedec_id;
	u8 silicon_rev;

	/* CRCs we need to check during a config update */
	struct axiom_crc {
		u32 runtime;
		u32 vltusageconfig;
		u32 nvltlusageconfig;
		u32 u22_sequencedata;
		u32 u43_hotspots;
		u32 u93_profiles;
		u32 u94_deltascalemap;
	} crc[AXIOM_CRC_NUM];

	bool cds_enabled;
	unsigned long enabled_slots;
	unsigned int num_slots;

	unsigned int max_report_byte_len;
	struct axiom_usage_table_entry {
		bool populated;
		unsigned int baseaddr;
		unsigned int size_bytes;
		const struct axiom_usage_info *info;
	} usage_table[AXIOM_MAX_USAGES];
};

static int axiom_u01_rev1_process_report(struct axiom_data *ts, const u8 *buf,
					 size_t bufsize);
static int axiom_u34_rev1_process_report(struct axiom_data *ts, const u8 *_buf,
					 size_t bufsize);
static int axiom_u41_rev2_process_report(struct axiom_data *ts, const u8 *buf,
					 size_t bufsize);

#define AXIOM_USAGE(num, rev)		\
	{				\
		.usage_num = num,	\
		.rev_num = rev,		\
	}

#define AXIOM_RO_USAGE(num, rev)	\
	{				\
		.usage_num = num,	\
		.rev_num = rev,		\
		.is_ro = true,		\
	}

#define AXIOM_CDU_USAGE(num, rev)	\
	{				\
		.usage_num = num,	\
		.rev_num = rev,		\
		.is_cdu = true,		\
	}

#define AXIOM_REPORT_USAGE(num, rev, func)	\
	{					\
		.usage_num = num,		\
		.rev_num = rev,			\
		.process_report = func,		\
	}

/*
 * All usages used by driver must be added to this list to ensure the
 * correct communictation with the devices. The list can contain multiple
 * entries of the same usage to handle different usage revisions.
 *
 * Note: During a th2cfgbin update the driver may use usages not listed here.
 * Therefore the th2cfgbin update compares the current running FW again the
 * th2cfgbin targets FW.
 */
static const struct axiom_usage_info driver_required_usages[] = {
	AXIOM_REPORT_USAGE(AXIOM_U01, 1, axiom_u01_rev1_process_report),
	AXIOM_USAGE(AXIOM_U02, 1),
	AXIOM_USAGE(AXIOM_U02, 2),
	AXIOM_USAGE(AXIOM_U04, 1),
	AXIOM_CDU_USAGE(AXIOM_U05, 1),
	AXIOM_CDU_USAGE(AXIOM_U22, 1),
	AXIOM_RO_USAGE(AXIOM_U31, 1),
	AXIOM_RO_USAGE(AXIOM_U32, 1),
	AXIOM_RO_USAGE(AXIOM_U33, 2),
	AXIOM_RO_USAGE(AXIOM_U36, 1),
	AXIOM_REPORT_USAGE(AXIOM_U34, 1, axiom_u34_rev1_process_report),
	AXIOM_REPORT_USAGE(AXIOM_U41, 2, axiom_u41_rev2_process_report),
	AXIOM_USAGE(AXIOM_U42, 1),
	AXIOM_CDU_USAGE(AXIOM_U43, 1),
	AXIOM_USAGE(AXIOM_U64, 2),
	AXIOM_CDU_USAGE(AXIOM_U77, 1),
	AXIOM_RO_USAGE(AXIOM_U82, 1),
	AXIOM_CDU_USAGE(AXIOM_U93, 1),
	AXIOM_CDU_USAGE(AXIOM_U94, 1),
	{ /* sentinel */ }
};

/************************ Common helpers **************************************/

static void axiom_set_runmode(struct axiom_data *ts, enum axiom_runmode mode)
{
	ts->mode = mode;
}

static enum axiom_runmode axiom_get_runmode(struct axiom_data *ts)
{
	return ts->mode;
}

static const char *axiom_runmode_to_string(struct axiom_data *ts)
{
	switch (ts->mode) {
	case AXIOM_DISCOVERY_MODE:	return "discovery";
	case AXIOM_TCP_MODE:		return "tcp";
	case AXIOM_TCP_CFG_UPDATE_MODE:	return "th2cfg-update";
	case AXIOM_BLP_PRE_MODE:	return "bootloader-pre";
	case AXIOM_BLP_MODE:		return "bootlaoder";
	default:			return "unknown";
	}
}

static bool axiom_skip_usage_check(struct axiom_data *ts)
{
	switch (ts->mode) {
	case AXIOM_TCP_CFG_UPDATE_MODE:
	case AXIOM_DISCOVERY_MODE:
	case AXIOM_BLP_MODE:
		return true;
	case AXIOM_BLP_PRE_MODE:
	case AXIOM_TCP_MODE:
	default:
		return false;
	}
}

static unsigned int
axiom_usage_baseaddr(struct axiom_data *ts, unsigned char usage_num)
{
	return ts->usage_table[usage_num].baseaddr;
}

static unsigned int
axiom_usage_size(struct axiom_data *ts, unsigned char usage_num)
{
	return ts->usage_table[usage_num].size_bytes;
}

static int
axiom_usage_rev(struct axiom_data *ts, unsigned char usage_num)
{
	struct axiom_usage_table_entry *entry = &ts->usage_table[usage_num];

	if (!entry->info)
		return -EINVAL;

	return entry->info->rev_num;
}

static bool
axiom_usage_entry_is_report(struct axiom_u31_usage_table_entry *entry)
{
	return entry->num_pages == 0;
}

static unsigned int
axiom_get_usage_size_bytes(struct axiom_u31_usage_table_entry *entry)
{
	unsigned char max_offset;

	max_offset = FIELD_GET(AXIOM_U31_REV1_MAX_OFFSET_MASK,
			       entry->max_offset) + 1;
	max_offset *= 2;

	if (axiom_usage_entry_is_report(entry))
		return max_offset;

	if (FIELD_GET(AXIOM_U31_REV1_OFFSET_TYPE_MASK, entry->max_offset))
		return (entry->num_pages - 1) * AXIOM_PAGE_BYTE_LEN + max_offset;

	return max_offset;
}

static void axiom_dump_usage_entry(struct device *dev,
				   struct axiom_u31_usage_table_entry *entry)
{
	unsigned int page_len, total_len;

	total_len = axiom_get_usage_size_bytes(entry);

	if (total_len > AXIOM_PAGE_BYTE_LEN)
		page_len = AXIOM_PAGE_BYTE_LEN;
	else
		page_len = total_len;

	if (axiom_usage_entry_is_report(entry))
		dev_dbg(dev,
			"u%02X rev.%d total-len:%u [REPORT]\n",
			entry->usage_num, entry->uifrevision, total_len);
	else
		dev_dbg(dev,
			"u%02X rev.%d first-page:%#02x page-len:%u num-pages:%u total-len:%u\n",
			entry->usage_num, entry->uifrevision, entry->start_page, page_len,
			entry->num_pages, total_len);
}

static const struct axiom_usage_info *
axiom_get_usage_info(struct axiom_u31_usage_table_entry *query)
{
	const struct axiom_usage_info *info = driver_required_usages;
	bool required = false;
	bool found = false;

	for (; info->usage_num; info++) {
		/* Skip all usages not used by the driver */
		if (query->usage_num != info->usage_num)
			continue;

		/* The usage is used so we need to mark it as required */
		required = true;

		/* Continue with the next usage if the revision doesn't match */
		if (query->uifrevision != info->rev_num)
			continue;

		found = true;
		break;
	}

	if (required && !found)
		return ERR_PTR(-EINVAL);

	return found ? info : NULL;
}

static bool axiom_usage_supported(struct axiom_data *ts, unsigned int baseaddr)
{
	struct axiom_usage_table_entry *entry;
	unsigned int i;

	if (axiom_skip_usage_check(ts))
		return true;

	dev_dbg(ts->dev, "Checking support for baseaddr: %#x\n", baseaddr);

	for (i = 0; i < ARRAY_SIZE(ts->usage_table); i++) {
		entry = &ts->usage_table[i];

		if (!entry->populated)
			continue;

		if (entry->baseaddr != baseaddr)
			continue;

		break;
	}

	if (i == ARRAY_SIZE(ts->usage_table)) {
		dev_warn(ts->dev, "Usage not found\n");
		return false;
	}

	if (!entry->info)
		WARN(1, "Unsupported usage u%x used, driver bug!", i);

	return !!entry->info;
}

static void axiom_poll(struct input_dev *input);

static unsigned long
axiom_wait_for_completion_timeout(struct axiom_data *ts, struct axiom_completion *x,
				  long timeout)
{
	struct i2c_client *client = to_i2c_client(ts->dev);
	unsigned long poll_timeout;

	if (client->irq)
		return wait_for_completion_timeout(&x->completion, timeout);

	/*
	 * Only firmware update cases do wait for completion. Since they require
	 * the input device to be closed, the poller is not running. So we need
	 * to do the polling manually.
	 */
	poll_timeout = timeout / 10;

	/*
	 * Very basic and not very accurate but it does the job because there
	 * are no known timeout constraints.
	 */
	do {
		axiom_poll(ts->input);
		fsleep(jiffies_to_usecs(poll_timeout));
		if (x->poll_done)
			break;
		timeout -= poll_timeout;
	} while (timeout > 0);

	x->poll_done = false;

	return timeout > 0 ? timeout : 0;
}

static void axiom_complete(struct axiom_data *ts, struct axiom_completion *x)
{
	struct i2c_client *client = to_i2c_client(ts->dev);

	if (client->irq)
		complete(&x->completion);
	else
		x->poll_done = true;
}

/*************************** Usage handling ***********************************/
/*
 * Wrapper functions to handle the usage access. Wrappers are used to add
 * different revision handling later on more easily.
 */
static int axiom_u02_wait_idle(struct axiom_data *ts)
{
	struct device *dev = ts->dev;
	unsigned int reg;
	int ret, _ret;
	u16 cmd;

	if (axiom_usage_rev(ts, AXIOM_U02) != 1 &&
	    axiom_usage_rev(ts, AXIOM_U02) != 2) {
		dev_err(dev, "Only u02 rev.1 and rev.2 are supported at the moment\n");
		return -EINVAL;
	}

	reg = axiom_usage_baseaddr(ts, AXIOM_U02);
	reg += AXIOM_U02_REV1_COMMAND_REG;

	/*
	 * Missing regmap_raw_read_poll_timeout for now. RESP_SUCCESS means that
	 * the last command successfully completed and the device is idle.
	 */
	ret = read_poll_timeout(regmap_raw_read, _ret,
				_ret || cmd == AXIOM_U02_REV1_RESP_SUCCESS,
				10 * USEC_PER_MSEC, 1 * USEC_PER_SEC, false,
				ts->regmap, reg, &cmd, 2);
	if (ret)
		dev_err(ts->dev, "Poll u02 timedout with: %#x\n", cmd);

	return ret;
}

static int
axiom_u02_send_msg(struct axiom_data *ts,
		   const struct axiom_u02_rev1_system_manager_msg *msg,
		   bool validate_response)
{
	struct device *dev = ts->dev;
	unsigned int reg;
	int ret;

	if (axiom_usage_rev(ts, AXIOM_U02) != 1 &&
	    axiom_usage_rev(ts, AXIOM_U02) != 2) {
		dev_err(dev, "Only u02 rev.1 and rev.2 are supported at the moment\n");
		return -EINVAL;
	}

	reg = axiom_usage_baseaddr(ts, AXIOM_U02);
	reg += AXIOM_U02_REV1_COMMAND_REG;

	ret = regmap_raw_write(ts->regmap, reg, msg, sizeof(*msg));
	if (ret)
		return ret;

	if (!validate_response)
		return 0;

	return axiom_u02_wait_idle(ts);
}

static int
axiom_u02_rev1_send_single_cmd(struct axiom_data *ts, u16 cmd)
{
	struct axiom_u02_rev1_system_manager_msg msg = {
		.command = cpu_to_le16(cmd)
	};

	return axiom_u02_send_msg(ts, &msg, true);
}

static int axiom_u02_handshakenvm(struct axiom_data *ts)
{
	return axiom_u02_rev1_send_single_cmd(ts, AXIOM_U02_REV1_CMD_HANDSHAKENVM);
}

static int axiom_u02_computecrc(struct axiom_data *ts)
{
	return axiom_u02_rev1_send_single_cmd(ts, AXIOM_U02_REV1_CMD_COMPUTECRCS);
}

static int axiom_u02_stop(struct axiom_data *ts)
{
	return axiom_u02_rev1_send_single_cmd(ts, AXIOM_U02_REV1_CMD_STOP);
}

static int axiom_u02_save_config(struct axiom_data *ts)
{
	struct axiom_u02_rev1_system_manager_msg msg;
	struct device *dev = ts->dev;
	int ret;

	if (axiom_usage_rev(ts, AXIOM_U02) != 1 &&
	    axiom_usage_rev(ts, AXIOM_U02) != 2) {
		dev_err(dev, "Only u02 rev.1 and rev.2 are supported at the moment\n");
		return -EINVAL;
	}

	msg.command = cpu_to_le16(AXIOM_U02_REV1_CMD_SAVEVLTLCFG2NVM);
	msg.parameters[0] = 0; /* Don't care */
	msg.parameters[1] = cpu_to_le16(AXIOM_U02_REV1_PARAM1_SAVEVLTLCFG2NVM);
	msg.parameters[2] = cpu_to_le16(AXIOM_U02_REV1_PARAM2_SAVEVLTLCFG2NVM);

	ret = axiom_u02_send_msg(ts, &msg, false);
	if (ret)
		return ret;

	/* Downstream axcfg.py waits for 2sec without checking U01 response */
	ret = axiom_wait_for_completion_timeout(ts, &ts->nvm_write,
					msecs_to_jiffies(2 * MSEC_PER_SEC));
	if (!ret)
		dev_err(ts->dev, "Error save volatile config timedout\n");

	return ret ? 0 : -ETIMEDOUT;
}

static int axiom_u02_swreset(struct axiom_data *ts)
{
	struct axiom_u02_rev1_system_manager_msg msg = { };
	struct device *dev = ts->dev;
	int ret;

	if (axiom_usage_rev(ts, AXIOM_U02) != 1 &&
	    axiom_usage_rev(ts, AXIOM_U02) != 2) {
		dev_err(dev, "Only u02 rev.1 and rev.2 are supported at the moment\n");
		return -EINVAL;
	}

	msg.command = cpu_to_le16(AXIOM_U02_REV1_CMD_SOFTRESET);
	ret = axiom_u02_send_msg(ts, &msg, false);
	if (ret)
		return ret;

	/*
	 * Downstream axcfg.py waits for 1sec without checking U01 hello. Tests
	 * showed that waiting for the hello message isn't enough therefore we
	 * need both to make it robuster.
	 */
	ret = axiom_wait_for_completion_timeout(ts, &ts->boot_complete,
					msecs_to_jiffies(1 * MSEC_PER_SEC));
	if (!ret)
		dev_err(ts->dev, "Error swreset timedout\n");

	fsleep(USEC_PER_SEC);

	return ret ? 0 : -ETIMEDOUT;
}

static int axiom_u02_fillconfig(struct axiom_data *ts)
{
	struct axiom_u02_rev1_system_manager_msg msg;
	struct device *dev = ts->dev;

	if (axiom_usage_rev(ts, AXIOM_U02) != 1 &&
	    axiom_usage_rev(ts, AXIOM_U02) != 2) {
		dev_err(dev, "Only u02 rev.1 and rev.2 are supported at the moment\n");
		return -EINVAL;
	}

	msg.command = cpu_to_le16(AXIOM_U02_REV1_CMD_FILLCONFIG);
	msg.parameters[0] = cpu_to_le16(AXIOM_U02_REV1_PARAM0_FILLCONFIG);
	msg.parameters[1] = cpu_to_le16(AXIOM_U02_REV1_PARAM1_FILLCONFIG);
	msg.parameters[2] = cpu_to_le16(AXIOM_U02_REV1_PARAM2_FILLCONFIG_ZERO);

	return axiom_u02_send_msg(ts, &msg, true);
}

static int axiom_u02_enter_bootloader(struct axiom_data *ts)
{
	struct axiom_u02_rev1_system_manager_msg msg = { };
	struct device *dev = ts->dev;
	unsigned int val;
	int ret;

	if (axiom_usage_rev(ts, AXIOM_U02) != 1 &&
	    axiom_usage_rev(ts, AXIOM_U02) != 2) {
		dev_err(dev, "Only u02 rev.1 and rev.2 are supported at the moment\n");
		return -EINVAL;
	}

	/*
	 * Enter the bootloader mode requires 3 consecutive messages so we can't
	 * check for the response.
	 */
	msg.command = cpu_to_le16(AXIOM_U02_REV1_CMD_ENTERBOOTLOADER);
	msg.parameters[0] = cpu_to_le16(AXIOM_U02_REV1_PARAM0_ENTERBOOLOADER_KEY1);
	ret = axiom_u02_send_msg(ts, &msg, false);
	if (ret) {
		dev_err(dev, "Failed to send bootloader-key1: %d\n", ret);
		return ret;
	}

	msg.parameters[0] = cpu_to_le16(AXIOM_U02_REV1_PARAM0_ENTERBOOLOADER_KEY2);
	ret = axiom_u02_send_msg(ts, &msg, false);
	if (ret) {
		dev_err(dev, "Failed to send bootloader-key2: %d\n", ret);
		return ret;
	}

	msg.parameters[0] = cpu_to_le16(AXIOM_U02_REV1_PARAM0_ENTERBOOLOADER_KEY3);
	ret = axiom_u02_send_msg(ts, &msg, false);
	if (ret) {
		dev_err(dev, "Failed to send bootloader-key3: %d\n", ret);
		return ret;
	}

	/* Sleep before the first read to give the device time */
	fsleep(250 * USEC_PER_MSEC);

	/* Wait till the device reports it is in bootloader mode */
	return regmap_read_poll_timeout(ts->regmap,
			AXIOM_U31_REV1_DEVICE_ID_HIGH_REG, val,
			FIELD_GET(AXIOM_U31_REV1_MODE_MASK, val) ==
			AXIOM_U31_REV1_MODE_BLP, 250 * USEC_PER_MSEC,
			USEC_PER_SEC);
}

static int axiom_u04_get(struct axiom_data *ts, u8 **_buf)
{
	u8 buf[AXIOM_U04_REV1_SIZE_BYTES];
	struct device *dev = ts->dev;
	unsigned int reg;
	int ret;

	if (axiom_usage_rev(ts, AXIOM_U04) != 1) {
		dev_err(dev, "Only u04 rev.1 is supported at the moment\n");
		return -EINVAL;
	}

	reg = axiom_usage_baseaddr(ts, AXIOM_U04);
	ret = regmap_raw_read(ts->regmap, reg, buf, sizeof(buf));
	if (ret)
		return ret;

	*_buf = kmemdup(buf, sizeof(buf), GFP_KERNEL);

	return sizeof(buf);
}

static int axiom_u04_set(struct axiom_data *ts, u8 *buf, unsigned int bufsize)
{
	struct device *dev = ts->dev;
	unsigned int reg;

	if (axiom_usage_rev(ts, AXIOM_U04) != 1) {
		dev_err(dev, "Only u04 rev.1 is supported at the moment\n");
		return -EINVAL;
	}

	reg = axiom_usage_baseaddr(ts, AXIOM_U04);
	return regmap_raw_write(ts->regmap, reg, buf, bufsize);
}

/*
 * U31 revision must be always rev.1 else the whole self discovery mechanism
 * fall apart.
 */
static int axiom_u31_parse_device_info(struct axiom_data *ts)
{
	struct regmap *regmap = ts->regmap;
	unsigned int id_low, id_high, val;
	int ret;

	ret = regmap_read(regmap, AXIOM_U31_REV1_DEVICE_ID_HIGH_REG, &id_high);
	if (ret)
		return ret;
	id_high = FIELD_GET(AXIOM_U31_REV1_DEVICE_ID_HIGH_MASK, id_high);

	ret = regmap_read(regmap, AXIOM_U31_REV1_DEVICE_ID_LOW_REG, &id_low);
	if (ret)
		return ret;
	ts->device_id = id_high << 8 | id_low;

	ret = regmap_read(regmap, AXIOM_U31_REV1_RUNTIME_FW_MAJ_REG, &val);
	if (ret)
		return ret;
	ts->fw_major = val;

	ret = regmap_read(regmap, AXIOM_U31_REV1_RUNTIME_FW_MIN_REG, &val);
	if (ret)
		return ret;
	ts->fw_minor = val;

	/* All other fields are not allowed to be read in BLP mode */
	if (axiom_get_runmode(ts) == AXIOM_BLP_MODE)
		return 0;

	ret = regmap_read(regmap, AXIOM_U31_REV1_RUNTIME_FW_RC_REG, &val);
	if (ret)
		return ret;
	ts->fw_rc = FIELD_GET(AXIOM_U31_REV1_RUNTIME_FW_RC_MASK, val);
	ts->silicon_rev = FIELD_GET(AXIOM_U31_REV1_SILICON_REV_MASK, val);

	ret = regmap_read(regmap, AXIOM_U31_REV1_RUNTIME_FW_STATUS_REG, &val);
	if (ret)
		return ret;
	ts->fw_status = FIELD_GET(AXIOM_U31_REV1_RUNTIME_FW_STATUS, val);

	ret = regmap_read(regmap, AXIOM_U31_REV1_JEDEC_ID_HIGH_REG, &val);
	if (ret)
		return ret;
	ts->jedec_id = val << 8;

	ret = regmap_read(regmap, AXIOM_U31_REV1_JEDEC_ID_LOW_REG, &val);
	if (ret)
		return ret;
	ts->jedec_id |= val;

	return 0;
}

static int axiom_u33_read(struct axiom_data *ts, struct axiom_crc *crc);

static int axiom_u31_device_discover(struct axiom_data *ts)
{
	struct axiom_u31_usage_table_entry *u31_usage_table __free(kfree) = NULL;
	struct axiom_u31_usage_table_entry *entry;
	struct regmap *regmap = ts->regmap;
	unsigned int mode, num_usages;
	struct device *dev = ts->dev;
	unsigned int i;
	int ret;

	axiom_set_runmode(ts, AXIOM_DISCOVERY_MODE);

	ret = regmap_read(regmap, AXIOM_U31_REV1_DEVICE_ID_HIGH_REG, &mode);
	if (ret) {
		dev_err(dev, "Failed to read MODE\n");
		return ret;
	}

	/* Abort if the device is in bootloader protocol mode */
	mode = FIELD_GET(AXIOM_U31_REV1_MODE_MASK, mode);
	if (mode == AXIOM_U31_REV1_MODE_BLP)
		axiom_set_runmode(ts, AXIOM_BLP_MODE);

	/* Since we are not in bootloader mode we can parse the device info */
	ret = axiom_u31_parse_device_info(ts);
	if (ret) {
		dev_err(dev, "Failed to parse device info\n");
		return ret;
	}

	/* All other fields are not allowed to be read in BLP mode */
	if (axiom_get_runmode(ts) == AXIOM_BLP_MODE) {
		dev_info(dev, "Device in Bootloader mode, firmware upload required\n");
		return -EACCES;
	}

	ret = regmap_read(regmap, AXIOM_U31_REV1_NUM_USAGES_REG, &num_usages);
	if (ret) {
		dev_err(dev, "Failed to read NUM_USAGES\n");
		return ret;
	}

	u31_usage_table = kcalloc(num_usages, sizeof(*u31_usage_table),
				  GFP_KERNEL);
	if (!u31_usage_table)
		return -ENOMEM;

	ret = regmap_raw_read(regmap, AXIOM_U31_REV1_PAGE1, u31_usage_table,
			      array_size(num_usages, sizeof(*u31_usage_table)));
	if (ret) {
		dev_err(dev, "Failed to read NUM_USAGES\n");
		return ret;
	}

	/*
	 * axiom_u31_device_discover() is call after fw update too, so ensure
	 * that the usage_table is cleared.
	 */
	memset(ts->usage_table, 0, sizeof(ts->usage_table));

	for (i = 0, entry = u31_usage_table; i < num_usages; i++, entry++) {
		unsigned char idx = entry->usage_num;
		const struct axiom_usage_info *info;
		unsigned int size_bytes;

		axiom_dump_usage_entry(dev, entry);

		/*
		 * Verify that the driver used usages are supported. Don't abort
		 * yet if a usage isn't supported to allow the user to dump the
		 * actual usage table.
		 */
		info = axiom_get_usage_info(entry);
		if (IS_ERR(info)) {
			dev_info(dev, "Required usage u%02x isn't supported for rev.%u\n",
				 entry->usage_num, entry->uifrevision);
			ret = -EACCES;
		}

		size_bytes = axiom_get_usage_size_bytes(entry);

		ts->usage_table[idx].baseaddr = entry->start_page << 8;
		ts->usage_table[idx].size_bytes = size_bytes;
		ts->usage_table[idx].populated = true;
		ts->usage_table[idx].info = info;

		if (axiom_usage_entry_is_report(entry) &&
		    ts->max_report_byte_len < size_bytes)
			ts->max_report_byte_len = size_bytes;
	}

	if (ret)
		return ret;

	/* From now on we are in TCP mode to include usage revision checks */
	axiom_set_runmode(ts, AXIOM_TCP_MODE);

	return axiom_u33_read(ts, &ts->crc[AXIOM_CRC_CUR]);
}

static int axiom_u33_read(struct axiom_data *ts, struct axiom_crc *crc)
{
	struct device *dev = ts->dev;
	struct axiom_u33_rev2 val;
	unsigned int reg;
	int ret;

	if (axiom_usage_rev(ts, AXIOM_U33) != 2) {
		dev_err(dev, "Only u33 rev.2 is supported at the moment\n");
		return -EINVAL;
	}

	reg = axiom_usage_baseaddr(ts, AXIOM_U33);
	ret = regmap_raw_read(ts->regmap, reg, &val, sizeof(val));
	if (ret) {
		dev_err(dev, "Failed to read u33\n");
		return ret;
	}

	crc->runtime = le32_to_cpu(val.runtime_crc);
	crc->vltusageconfig = le32_to_cpu(val.vltusageconfig_crc);
	crc->nvltlusageconfig = le32_to_cpu(val.nvltlusageconfig_crc);
	crc->u22_sequencedata = le32_to_cpu(val.u22_sequencedata_crc);
	crc->u43_hotspots = le32_to_cpu(val.u43_hotspots_crc);
	crc->u93_profiles = le32_to_cpu(val.u93_profiles_crc);
	crc->u94_deltascalemap = le32_to_cpu(val.u94_deltascalemap_crc);

	return 0;
}

static void axiom_u42_get_touchslots(struct axiom_data *ts)
{
	u8 *buf __free(kfree) = NULL;
	struct device *dev = ts->dev;
	unsigned int bufsize;
	unsigned int reg;
	int ret, i;

	if (axiom_usage_rev(ts, AXIOM_U42) != 1) {
		dev_warn(dev, "Unsupported u42 revision, use default value\n");
		goto fallback;
	}

	bufsize = axiom_usage_size(ts, AXIOM_U42);
	buf = kzalloc(bufsize, GFP_KERNEL);
	if (!buf) {
		dev_warn(dev, "Failed to alloc u42 read buffer, use default value\n");
		goto fallback;
	}

	reg = axiom_usage_baseaddr(ts, AXIOM_U42);
	ret = regmap_raw_read(ts->regmap, reg, buf, bufsize);
	if (ret) {
		dev_warn(dev, "Failed to read u42, use default value\n");
		goto fallback;
	}

	ts->enabled_slots = 0;
	ts->num_slots = 0;

	for (i = 0; i < AXIOM_MAX_TOUCHSLOTS; i++) {
		bool touch_enabled;

		touch_enabled = buf[AXIOM_U42_REV1_REPORT_ID_CONTAINS(i)] ==
				AXIOM_U42_REV1_REPORT_ID_TOUCH;
		if (touch_enabled) {
			ts->enabled_slots |= BIT(i);
			ts->num_slots++;
		}
	}

	return;

fallback:
	ts->enabled_slots = AXIOM_MAX_TOUCHSLOTS_MASK;
	ts->num_slots = AXIOM_MAX_TOUCHSLOTS;
}

static void axiom_u64_cds_enabled(struct axiom_data *ts)
{
	unsigned int reg, val;
	int ret;

	if (axiom_usage_rev(ts, AXIOM_U64) != 2)
		goto fallback_out;

	reg = axiom_usage_baseaddr(ts, AXIOM_U64);
	reg += AXIOM_U64_REV2_ENABLECDSPROCESSING_REG;

	ret = regmap_read(ts->regmap, reg, &val);
	if (ret)
		goto fallback_out;

	val = FIELD_GET(AXIOM_U64_REV2_ENABLECDSPROCESSING_MASK, val);
	ts->cds_enabled = val ? true : false;

	return;

fallback_out:
	ts->cds_enabled = false;
}

static int axiom_cdu_wait_idle(struct axiom_data *ts, u8 cdu_usage_num)
{
	unsigned int reg;
	int ret, _ret;
	u16 cmd;

	reg = axiom_usage_baseaddr(ts, cdu_usage_num);

	/*
	 * Missing regmap_raw_read_poll_timeout for now. RESP_SUCCESS means that
	 * the last command successfully completed and the device is idle.
	 */
	ret = read_poll_timeout(regmap_raw_read, _ret,
				_ret || cmd == AXIOM_CDU_RESP_SUCCESS,
				10 * USEC_PER_MSEC, 1 * USEC_PER_SEC, false,
				ts->regmap, reg, &cmd, 2);
	if (ret)
		dev_err(ts->dev, "Poll CDU u%x timedout with: %#x\n",
			cdu_usage_num, cmd);

	return ret;
}

/*********************** Report usage handling ********************************/

static int axiom_process_report(struct axiom_data *ts, unsigned char usage_num,
				const u8 *buf, size_t buflen)
{
	struct axiom_usage_table_entry *entry = &ts->usage_table[usage_num];

	/* Skip processing if not in TCP mode */
	if ((axiom_get_runmode(ts) != AXIOM_TCP_MODE) &&
	    (axiom_get_runmode(ts) != AXIOM_TCP_CFG_UPDATE_MODE))
		return 0;

	/* May happen if an unsupported usage was requested */
	if (!entry) {
		dev_info(ts->dev, "Unsupported usage U%x request\n", usage_num);
		return 0;
	}

	/* Supported report usages need to have a process_report hook */
	if (!entry->info || !entry->info->process_report)
		return -EINVAL;

	return entry->info->process_report(ts, buf, buflen);
}

/* Make use of datasheet method 1 - single transfer read */
static int
axiom_u34_rev1_process_report(struct axiom_data *ts, const u8 *_buf, size_t bufsize)
{
	unsigned int reg = axiom_usage_baseaddr(ts, AXIOM_U34);
	struct regmap *regmap = ts->regmap;
	u8 buf[AXIOM_PAGE_BYTE_LEN] = { };
	struct device *dev = ts->dev;
	unsigned char report_usage;
	u16 crc_report, crc_calc;
	unsigned int len;
	u8 *payload;
	int ret;

	ret = regmap_raw_read(regmap, reg, buf, ts->max_report_byte_len);
	if (ret)
		return ret;

	/* TODO: Add overflow statistics */

	/* REPORTLENGTH is in uint16 */
	len = FIELD_GET(AXIOM_U34_REV1_REPORTLENGTH_MASK, buf[0]);
	len *= 2;

	/*
	 * Downstream ignores zero length reports, extend the check to validate
	 * the upper bound too.
	 */
	if (len == 0 || len > AXIOM_PAGE_BYTE_LEN) {
		dev_dbg_ratelimited(dev, "Invalid report length: %u\n", len);
		return -EINVAL;
	}

	/*
	 * The CRC16 value can be queried at the last two bytes of the report.
	 * The value itself is covering the complete report excluding the CRC16
	 * value at the end.
	 */
	crc_report = get_unaligned_le16(&buf[len - 2]);
	crc_calc = crc16(0, buf, (len - 2));

	if (crc_calc != crc_report) {
		dev_err_ratelimited(dev, "CRC16 mismatch!\n");
		return -EINVAL;
	}

	report_usage = buf[1];
	payload = &buf[AXIOM_U34_REV1_PREAMBLE_BYTES];
	len -= AXIOM_U34_REV1_PREAMBLE_BYTES - AXIOM_U34_REV1_POSTAMBLE_BYTES;

	switch (report_usage) {
	case AXIOM_U01:
	case AXIOM_U41:
		return axiom_process_report(ts, report_usage, payload, len);
	default:
		dev_dbg(dev, "Unsupported report u%02X received\n",
			report_usage);
	}

	return 0;
}

static void
axiom_u41_rev2_decode_target(const u8 *buf, u8 id, u16 *x, u16 *y, s8 *z)
{
	u16 val;

	val = get_unaligned_le16(&buf[AXIOM_U41_REV2_X_REG(id)]);
	val &= AXIOM_MAX_XY;
	*x = val;

	val = get_unaligned_le16(&buf[AXIOM_U41_REV2_Y_REG(id)]);
	val &= AXIOM_MAX_XY;
	*y = val;

	*z = buf[AXIOM_U41_REV2_Z_REG(id)];
}

static int
axiom_u41_rev2_process_report(struct axiom_data *ts, const u8 *buf, size_t bufsize)
{
	struct input_dev *input = ts->input;
	unsigned char id;
	u16 targets;

	/*
	 * The input registration can be postponed but the touchscreen FW is
	 * sending u41 reports regardless.
	 */
	if (!input)
		return 0;

	targets = get_unaligned_le16(&buf[AXIOM_U41_REV2_TARGETSTATUS_REG]);

	for_each_set_bit(id, &ts->enabled_slots, AXIOM_MAX_TOUCHSLOTS) {
		bool present;
		u16 x, y;
		s8 z;

		axiom_u41_rev2_decode_target(buf, id, &x, &y, &z);

		present = targets & BIT(id);
		/* Ignore possible jitters */
		if (z == AXIOM_PROX_LEVEL)
			present = false;

		dev_dbg(ts->dev, "id:%u x:%u y:%u z:%d present:%u",
			id, x, y, z, present);

		input_mt_slot(input, id);
		if (input_mt_report_slot_state(input, MT_TOOL_FINGER, present))
			touchscreen_report_pos(input, &ts->prop, x, y, true);

		if (!present)
			continue;

		input_report_abs(input, ABS_MT_DISTANCE, z < 0 ? -z : 0);
		if (ts->cds_enabled)
			input_report_abs(input, ABS_MT_PRESSURE, z >= 0 ? z : 0);
	}

	input_sync(input);

	return 0;
}

static int
axiom_u01_rev1_process_report(struct axiom_data *ts, const u8 *buf, size_t bufsize)
{
	switch (buf[AXIOM_U01_REV1_REPORTTYPE_REG]) {
	case AXIOM_U01_REV1_REPORTTYPE_HELLO:
		dev_dbg(ts->dev, "u01 HELLO received\n");
		axiom_complete(ts, &ts->boot_complete);
		return 0;
	case AXIOM_U01_REV1_REPORTTYPE_HEARTBEAT:
		dev_dbg_ratelimited(ts->dev, "u01 HEARTBEAT received\n");
		return 0;
	case AXIOM_U01_REV1_REPORTTYPE_OPCOMPLETE:
		dev_dbg(ts->dev, "u01 OPCOMPLETE received\n");
		axiom_u02_handshakenvm(ts);
		axiom_complete(ts, &ts->nvm_write);
		return 0;
	default:
		return -EINVAL;
	}
}

/**************************** Regmap handling *********************************/

#define AXIOM_CMD_HDR_DIR_MASK	BIT(15)
#define   AXIOM_CMD_HDR_READ	1
#define	  AXIOM_CMD_HDR_WRITE	0
#define AXIOM_CMD_HDR_LEN_MASK	GENMASK(14, 0)

struct axiom_cmd_header {
	__le16 target_address;
	__le16 xferlen;
};

/* Custom regmap read/write handling is required due to the aXiom protocol */
static int axiom_regmap_read(void *context, const void *reg_buf, size_t reg_size,
			     void *val_buf, size_t val_size)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	struct axiom_data *ts = i2c_get_clientdata(i2c);
	struct axiom_cmd_header hdr;
	u16 xferlen, addr, baseaddr;
	struct i2c_msg xfer[2];
	int ret;

	if (val_size > AXIOM_MAX_XFERLEN) {
		dev_err(ts->dev, "Exceed max xferlen: %zu > %u\n",
			val_size, AXIOM_MAX_XFERLEN);
		return -EINVAL;
	}

	addr = *((u16 *)reg_buf);
	hdr.target_address = cpu_to_le16(addr);
	xferlen = FIELD_PREP(AXIOM_CMD_HDR_DIR_MASK, AXIOM_CMD_HDR_READ) |
		  FIELD_PREP(AXIOM_CMD_HDR_LEN_MASK, val_size);
	hdr.xferlen = cpu_to_le16(xferlen);

	/* Verify that usage including the usage rev is supported */
	baseaddr = addr & AXIOM_USAGE_BASEADDR_MASK;
	if (!axiom_usage_supported(ts, baseaddr))
		return -EINVAL;

	xfer[0].addr = i2c->addr;
	xfer[0].flags = 0;
	xfer[0].len = sizeof(hdr);
	xfer[0].buf = (u8 *)&hdr;

	xfer[1].addr = i2c->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = val_size;
	xfer[1].buf = val_buf;

	ret = i2c_transfer(i2c->adapter, xfer, 2);
	if (ret == 2)
		return 0;
	else if (ret < 0)
		return ret;
	else
		return -EIO;
}

static int axiom_regmap_write(void *context, const void *data, size_t count)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	struct axiom_data *ts = i2c_get_clientdata(i2c);
	char *buf __free(kfree) = NULL;
	struct axiom_cmd_header hdr;
	u16 xferlen, addr, baseaddr;
	size_t val_size, msg_size;
	int ret;

	val_size = count - sizeof(addr);
	if (val_size > AXIOM_MAX_XFERLEN) {
		dev_err(ts->dev, "Exceed max xferlen: %zu > %u\n",
			val_size, AXIOM_MAX_XFERLEN);
		return -EINVAL;
	}

	addr = *((u16 *)data);
	hdr.target_address = cpu_to_le16(addr);
	xferlen = FIELD_PREP(AXIOM_CMD_HDR_DIR_MASK, AXIOM_CMD_HDR_WRITE) |
		  FIELD_PREP(AXIOM_CMD_HDR_LEN_MASK, val_size);
	hdr.xferlen = cpu_to_le16(xferlen);

	/* Verify that usage including the usage rev is supported */
	baseaddr = addr & AXIOM_USAGE_BASEADDR_MASK;
	if (!axiom_usage_supported(ts, baseaddr))
		return -EINVAL;

	msg_size = sizeof(hdr) + val_size;
	buf = kzalloc(msg_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(&buf[sizeof(hdr)], &((char *)data)[2], val_size);

	ret = i2c_master_send(i2c, buf, msg_size);

	return ret == msg_size ? 0 : ret;
}

static const struct regmap_config axiom_i2c_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.read = axiom_regmap_read,
	.write = axiom_regmap_write,
};

/************************ FW update handling **********************************/

static int axiom_update_input_dev(struct axiom_data *ts);

static enum fw_upload_err
axiom_axfw_fw_prepare(struct fw_upload *fw_upload, const u8 *data, u32 size)
{
	struct axiom_data *ts = fw_upload->dd_handle;
	struct axiom_firmware *afw = &ts->fw[AXIOM_FW_AXFW];
	u8 major_ver, minor_ver, rc_ver, status;
	u32 fw_file_crc32, crc32_calc;
	struct device *dev = ts->dev;
	unsigned int signature_len;
	enum fw_upload_err ret;
	u16 fw_file_format_ver;
	u16 fw_file_device_id;

	mutex_lock(&afw->lock);
	afw->cancel = false;
	mutex_unlock(&afw->lock);

	mutex_lock(&ts->fwupdate_lock);

	if (size < sizeof(struct axiom_fw_axfw_hdr)) {
		dev_err(dev, "Invalid AXFW file size\n");
		ret = FW_UPLOAD_ERR_INVALID_SIZE;
		goto out;
	}

	signature_len = strlen(AXIOM_FW_AXFW_SIGNATURE);
	if (strncmp(data, AXIOM_FW_AXFW_SIGNATURE, signature_len)) {
		/*
		 * AXFW has a header which can be used to perform validations,
		 * ALC don't. Therefore the AXFW format is preferred.
		 */
		dev_warn(dev, "No AXFW signature, assume ALC firmware\n");
		ret = FW_UPLOAD_ERR_NONE;
		goto out;
	}

	fw_file_crc32 = get_unaligned_le32(&data[signature_len]);
	crc32_calc = crc32(~0, &data[8], size - 8) ^ 0xffffffff;
	if (fw_file_crc32 != crc32_calc) {
		dev_err(dev, "AXFW CRC32 doesn't match (fw:%#x calc:%#x)\n",
			fw_file_crc32, crc32_calc);
		ret = FW_UPLOAD_ERR_FW_INVALID;
		goto out;
	}

	data += signature_len + sizeof(fw_file_crc32);
	fw_file_format_ver = get_unaligned_le16(data);
	if (fw_file_format_ver != AXIOM_FW_AXFW_FILE_FMT_VER) {
		dev_err(dev, "Invalid AXFW file format version: %04x",
			fw_file_format_ver);
		ret = FW_UPLOAD_ERR_FW_INVALID;
		goto out;
	}

	data += sizeof(fw_file_format_ver);
	fw_file_device_id = get_unaligned_le16(data);
	if (fw_file_device_id != ts->device_id) {
		dev_err(dev, "Invalid AXFW target device (fw:%#04x dev:%#04x)\n",
			fw_file_device_id, ts->device_id);
		ret = FW_UPLOAD_ERR_FW_INVALID;
		goto out;
	}

	/*
	 * This can happen if:
	 *  * the device came up in bootloader mode, or
	 *  * downloading the firmware failed in between, or
	 *  * the following usage discovery failed.
	 *
	 *  All cases are crcitical and we need to use any firmware to
	 *  bring the device back into a working state which is supported by the
	 *  host.
	 */
	if (axiom_get_runmode(ts) != AXIOM_TCP_MODE)
		return FW_UPLOAD_ERR_NONE;

	data += sizeof(fw_file_device_id);
	/* Skip variant */
	minor_ver = *++data;
	major_ver = *++data;
	rc_ver = *++data;
	status = *++data;

	if (major_ver == ts->fw_major && minor_ver == ts->fw_minor &&
	    rc_ver == ts->fw_rc && status == ts->fw_status) {
		ret = FW_UPLOAD_ERR_SKIP;
		goto out;
	}

	dev_info(dev, "Detected AXFW %02u.%02u.%02u (%s)\n",
		 major_ver, minor_ver, rc_ver,
		 status ? "production" : "engineering");

	mutex_lock(&afw->lock);
	ret = afw->cancel ? FW_UPLOAD_ERR_CANCELED : FW_UPLOAD_ERR_NONE;
	mutex_unlock(&afw->lock);

out:
	/*
	 * In FW_UPLOAD_ERR_NONE case the complete handler will release the
	 * lock.
	 */
	if (ret != FW_UPLOAD_ERR_NONE)
		mutex_unlock(&ts->fwupdate_lock);

	return ret;
}

static int axiom_enter_bootloader_mode(struct axiom_data *ts)
{
	struct device *dev = ts->dev;
	int ret;

	axiom_set_runmode(ts, AXIOM_BLP_PRE_MODE);

	ret = axiom_u02_wait_idle(ts);
	if (ret)
		goto err_out;

	ret = axiom_u02_enter_bootloader(ts);
	if (ret) {
		dev_err(dev, "Failed to enter bootloader mode\n");
		goto err_out;
	}

	axiom_set_runmode(ts, AXIOM_BLP_MODE);

	return 0;

err_out:
	axiom_set_runmode(ts, AXIOM_TCP_MODE);

	return ret;
}

static int axoim_blp_wait_ready(struct axiom_data *ts)
{
	struct device *dev = ts->dev;
	unsigned int reg;
	int tmp, ret;
	u8 buf[4];

	reg = AXIOM_U01_BLP_SATUS_REG;

	/* BLP busy poll requires to read 4 bytes! */
	ret = read_poll_timeout(regmap_raw_read, tmp,
				tmp || !(buf[2] & AXIOM_U01_BLP_STATUS_BUSY),
				10 * USEC_PER_MSEC, 5 * USEC_PER_SEC, false,
				ts->regmap, reg, &buf, 4);
	if (ret)
		dev_err(dev, "Bootloader wait processing packets failed %d\n", ret);

	return ret;
}

static int
axiom_blp_write_chunk(struct axiom_data *ts, const u8 *data, u16 length)
{
	unsigned int chunk_size = AXIOM_U01_BLP_FIFO_CHK_SIZE_BYTES;
	unsigned int reg = AXIOM_U01_BLP_FIFO_REG;
	struct device *dev = ts->dev;
	unsigned int pos = 0;
	int ret;

	ret = axoim_blp_wait_ready(ts);
	if (ret)
		return ret;

	/*
	 * TODO: Downstream does this chunk transfers. Verify if this is
	 * required if one fw-chunk <= AXIOM_MAX_XFERLEN
	 */
	while (pos < length) {
		u16 len;

		len = chunk_size;
		if ((pos + chunk_size) > length)
			len = length - pos;

		ret = regmap_raw_write(ts->regmap, reg, &data[pos], len);
		if (ret) {
			dev_err(dev, "Bootloader download AXFW chunk failed %d\n", ret);
			return ret;
		}

		pos += len;
		ret = axoim_blp_wait_ready(ts);
		if (ret)
			return ret;
	}

	return 0;
}

static int axiom_blp_reset(struct axiom_data *ts)
{
	__le16 reset_cmd = cpu_to_le16(AXIOM_U01_BLP_COMMAND_RESET);
	unsigned int reg = AXIOM_U01_BLP_COMMAND_REG;
	struct device *dev = ts->dev;
	unsigned int attempts = 20;
	unsigned int mode;
	int ret;

	ret = axoim_blp_wait_ready(ts);
	if (ret)
		return ret;

	/*
	 * For some reason this write fail with -ENXIO. Skip checking the return
	 * code (which is also done by the downstream axfw.py tool and poll u31
	 * instead.
	 */
	regmap_raw_write(ts->regmap, reg, &reset_cmd, sizeof(reset_cmd));

	do {
		ret = regmap_read(ts->regmap, AXIOM_U31_REV1_DEVICE_ID_HIGH_REG,
				  &mode);
		if (!ret)
			break;

		fsleep(250 * USEC_PER_MSEC);
	} while (attempts--);

	if (ret) {
		dev_err(dev, "Failed to read MODE after BLP reset: %d\n", ret);
		return ret;
	}

	mode = FIELD_GET(AXIOM_U31_REV1_MODE_MASK, mode);
	if (mode == AXIOM_U31_REV1_MODE_BLP) {
		dev_err(dev, "Device still in BLP mode, abort\n");
		return -EINVAL;
	}

	return 0;
}

static void axiom_lock_input_device(struct axiom_data *ts)
{
	if (!ts->input)
		return;

	mutex_lock(&ts->input->mutex);
}

static void axiom_unlock_input_device(struct axiom_data *ts)
{
	if (!ts->input)
		return;

	mutex_unlock(&ts->input->mutex);
}

static void axiom_unregister_input_dev(struct axiom_data *ts)
{
	if (ts->input)
		input_unregister_device(ts->input);

	ts->input = NULL;
}

static enum fw_upload_err
axiom_axfw_fw_write(struct fw_upload *fw_upload, const u8 *data, u32 offset,
		    u32 size, u32 *written)
{
	struct axiom_data *ts = fw_upload->dd_handle;
	struct axiom_firmware *afw = &ts->fw[AXIOM_FW_AXFW];
	struct device *dev = ts->dev;
	bool cancel;
	int ret;

	/* Done before cancel check due to cleanup based put */
	ret = pm_runtime_resume_and_get(ts->dev);
	if (ret)
		return FW_UPLOAD_ERR_HW_ERROR;

	mutex_lock(&afw->lock);
	cancel = afw->cancel;
	mutex_unlock(&afw->lock);

	if (cancel)
		return FW_UPLOAD_ERR_CANCELED;

	axiom_lock_input_device(ts);

	if (ts->input && input_device_enabled(ts->input)) {
		dev_err(dev, "Input device not idle, abort AXFW/ALC update\n");
		goto err;
	}

	if (!strncmp(data, AXIOM_FW_AXFW_SIGNATURE,
		     strlen(AXIOM_FW_AXFW_SIGNATURE))) {
		/* Set the pointer to the first fw chunk */
		data += sizeof(struct axiom_fw_axfw_hdr);
		size -= sizeof(struct axiom_fw_axfw_hdr);
		*written += sizeof(struct axiom_fw_axfw_hdr);
	}

	if (axiom_enter_bootloader_mode(ts))
		goto err;

	while (size) {
		u16 chunk_len, len;

		chunk_len = get_unaligned_be16(&data[6]);
		len = chunk_len + sizeof(struct axiom_fw_axfw_chunk_hdr);

		/*
		 * The bootlaoder FW can handle the complete chunk incl. the
		 * header.
		 */
		ret = axiom_blp_write_chunk(ts, data, len);
		if (ret)
			goto err;

		size -= len;
		*written += len;
		data += len;
	}

	ret = axiom_blp_reset(ts);
	if (ret)
		dev_warn(dev, "BLP reset failed\n");

	ret = axiom_u31_device_discover(ts);
	if (ret) {
		/*
		 * This is critical and we need to avoid that the user-space can
		 * still use the input-dev.
		 */
		axiom_unlock_input_device(ts);
		axiom_unregister_input_dev(ts);
		dev_err(dev, "Device discovery failed after AXFW/ALC firmware update\n");
		goto err;
	}

	/* Unlock before the input device gets unregistered */
	axiom_unlock_input_device(ts);

	ret = axiom_update_input_dev(ts);
	if (ret) {
		dev_err(dev, "Input device update failed after AXFW/ALC firmware update\n");
		return FW_UPLOAD_ERR_HW_ERROR;
	}

	dev_info(dev, "AXFW update successful\n");

	return FW_UPLOAD_ERR_NONE;

err:
	axiom_unlock_input_device(ts);
	return FW_UPLOAD_ERR_HW_ERROR;
}

static enum fw_upload_err axiom_fw_poll_complete(struct fw_upload *fw_upload)
{
	return FW_UPLOAD_ERR_NONE;
}

static void axiom_axfw_fw_cancel(struct fw_upload *fw_upload)
{
	struct axiom_data *ts = fw_upload->dd_handle;
	struct axiom_firmware *afw = &ts->fw[AXIOM_FW_AXFW];

	mutex_lock(&afw->lock);
	afw->cancel = true;
	mutex_unlock(&afw->lock);
}

static void axiom_axfw_fw_cleanup(struct fw_upload *fw_upload)
{
	struct axiom_data *ts = fw_upload->dd_handle;

	mutex_unlock(&ts->fwupdate_lock);
	pm_runtime_mark_last_busy(ts->dev);
	pm_runtime_put_sync_autosuspend(ts->dev);
}

static const struct fw_upload_ops axiom_axfw_fw_upload_ops = {
	.prepare = axiom_axfw_fw_prepare,
	.write = axiom_axfw_fw_write,
	.poll_complete = axiom_fw_poll_complete,
	.cancel = axiom_axfw_fw_cancel,
	.cleanup = axiom_axfw_fw_cleanup,
};

static int
axiom_set_new_crcs(struct axiom_data *ts, const struct axiom_fw_cfg_chunk *cfg)
{
	struct axiom_crc *crc = &ts->crc[AXIOM_CRC_NEW];
	const u32 *u33_data = (const u32 *)cfg->usage_content;

	if (cfg->usage_rev != 2) {
		dev_err(ts->dev, "The driver doesn't support u33 revision %u\n",
			cfg->usage_rev);
		return -EINVAL;
	}

	crc->runtime = get_unaligned_le32(u33_data);
	crc->nvltlusageconfig = get_unaligned_le32(&u33_data[3]);
	crc->vltusageconfig = get_unaligned_le32(&u33_data[4]);
	crc->u22_sequencedata = get_unaligned_le32(&u33_data[5]);
	crc->u43_hotspots = get_unaligned_le32(&u33_data[6]);
	crc->u93_profiles = get_unaligned_le32(&u33_data[7]);
	crc->u94_deltascalemap = get_unaligned_le32(&u33_data[8]);

	return 0;
}

static unsigned int
axiom_cfg_fw_prepare_chunk(struct axiom_fw_cfg_chunk *chunk, const u8 *data)
{
	chunk->usage_num = data[0];
	chunk->usage_rev = data[1];
	chunk->usage_length = get_unaligned_le16(&data[3]);
	chunk->usage_content = &data[5];

	return chunk->usage_length + sizeof(struct axiom_fw_cfg_chunk_hdr);
}

static bool axiom_cfg_fw_update_required(struct axiom_data *ts)
{
	struct axiom_crc *cur, *new;

	cur = &ts->crc[AXIOM_CRC_CUR];
	new = &ts->crc[AXIOM_CRC_NEW];

	if (cur->nvltlusageconfig != new->nvltlusageconfig ||
	    cur->u22_sequencedata != new->u22_sequencedata ||
	    cur->u43_hotspots != new->u43_hotspots ||
	    cur->u93_profiles != new->u93_profiles ||
	    cur->u94_deltascalemap != new->u94_deltascalemap)
		return true;

	return false;
}

static enum fw_upload_err
axiom_cfg_fw_prepare(struct fw_upload *fw_upload, const u8 *data, u32 size)
{
	struct axiom_data *ts = fw_upload->dd_handle;
	struct axiom_firmware *afw = &ts->fw[AXIOM_FW_CFG];
	u32 cur_runtime_crc, fw_runtime_crc;
	struct axiom_fw_cfg_chunk chunk;
	struct device *dev = ts->dev;
	enum fw_upload_err ret;
	u32 signature;

	mutex_lock(&afw->lock);
	afw->cancel = false;
	mutex_unlock(&afw->lock);

	mutex_lock(&ts->fwupdate_lock);

	if (axiom_get_runmode(ts) != AXIOM_TCP_MODE) {
		dev_err(dev, "Device not in TCP mode, abort TH2CFG update\n");
		ret = FW_UPLOAD_ERR_HW_ERROR;
		goto out;
	}

	if (size < sizeof(struct axiom_fw_cfg_hdr)) {
		dev_err(dev, "Invalid TH2CFG file size\n");
		ret = FW_UPLOAD_ERR_INVALID_SIZE;
		goto out;
	}

	signature = get_unaligned_be32(data);
	if (signature != AXIOM_FW_CFG_SIGNATURE) {
		dev_err(dev, "Invalid TH2CFG signature\n");
		ret = FW_UPLOAD_ERR_FW_INVALID;
		goto out;
	}

	/* Skip to the first fw chunk */
	data += sizeof(struct axiom_fw_cfg_hdr);
	size -= sizeof(struct axiom_fw_cfg_hdr);

	/*
	 * Search for u33 which contains the CRC information and perform only
	 * the runtime-crc check.
	 */
	while (size) {
		unsigned int chunk_len;

		chunk_len = axiom_cfg_fw_prepare_chunk(&chunk, data);
		if (chunk.usage_num == AXIOM_U33)
			break;

		data += chunk_len;
		size -= chunk_len;
	}

	if (size == 0) {
		dev_err(dev, "Failed to find the u33 entry in TH2CFG\n");
		ret = FW_UPLOAD_ERR_FW_INVALID;
		goto out;
	}

	ret = axiom_set_new_crcs(ts, &chunk);
	if (ret) {
		ret = FW_UPLOAD_ERR_FW_INVALID;
		goto out;
	}

	/*
	 * Nothing to do if the CRCs are the same. TODO: Must be extended once
	 * the CDU update is added.
	 */
	if (!axiom_cfg_fw_update_required(ts)) {
		ret = FW_UPLOAD_ERR_SKIP;
		goto out;
	}

	cur_runtime_crc = ts->crc[AXIOM_CRC_CUR].runtime;
	fw_runtime_crc = ts->crc[AXIOM_CRC_NEW].runtime;
	if (cur_runtime_crc != fw_runtime_crc) {
		dev_err(dev, "TH2CFG and device runtime CRC doesn't match: %#x != %#x\n",
			fw_runtime_crc, cur_runtime_crc);
		ret = FW_UPLOAD_ERR_FW_INVALID;
		goto out;
	}

	mutex_lock(&afw->lock);
	ret = afw->cancel ? FW_UPLOAD_ERR_CANCELED : FW_UPLOAD_ERR_NONE;
	mutex_unlock(&afw->lock);

out:
	/*
	 * In FW_UPLOAD_ERR_NONE case the complete handler will release the
	 * lock.
	 */
	if (ret != FW_UPLOAD_ERR_NONE)
		mutex_unlock(&ts->fwupdate_lock);

	return ret;
}

static int axiom_zero_volatile_mem(struct axiom_data *ts)
{
	int ret, size;
	u8 *buf;

	/* Zero out the volatile memory except for the user content in u04 */
	ret = axiom_u04_get(ts, &buf);
	if (ret < 0)
		return ret;
	size = ret;

	ret = axiom_u02_fillconfig(ts);
	if (ret)
		goto out;

	ret = axiom_u04_set(ts, buf, size);
out:
	kfree(buf);
	return ret;
}

static bool
axiom_skip_cfg_chunk(struct axiom_data *ts, const struct axiom_fw_cfg_chunk *chunk)
{
	u8 usage_num = chunk->usage_num;

	if (!ts->usage_table[usage_num].populated) {
		dev_warn(ts->dev, "Unknown usage chunk for u%#x\n", usage_num);
		return true;
	}

	/* Skip read-only usages */
	if (ts->usage_table[usage_num].info &&
	    ts->usage_table[usage_num].info->is_ro)
		return true;

	return false;
}

static int
axiom_write_cdu_usage(struct axiom_data *ts, const struct axiom_fw_cfg_chunk *chunk)
{
	struct axiom_cdu_usage cdu = { };
	struct device *dev = ts->dev;
	unsigned int remaining;
	unsigned int reg;
	unsigned int pos;
	int ret;

	pos = 0;
	remaining = chunk->usage_length;
	cdu.command = cpu_to_le16(AXIOM_CDU_CMD_STORE);
	reg = axiom_usage_baseaddr(ts, chunk->usage_num);

	while (remaining) {
		unsigned int size;

		cdu.parameters[1] = cpu_to_le16(pos);

		size = remaining;
		if (size > AXIOM_CDU_MAX_DATA_BYTES)
			size = AXIOM_CDU_MAX_DATA_BYTES;

		memset(cdu.data, 0, sizeof(cdu.data));
		memcpy(cdu.data, &chunk->usage_content[pos], size);

		ret = regmap_raw_write(ts->regmap, reg, &cdu, sizeof(cdu));
		if (ret) {
			dev_err(dev, "Failed to write CDU u%x\n",
				chunk->usage_num);
			return ret;
		}

		ret = axiom_cdu_wait_idle(ts, chunk->usage_num);
		if (ret) {
			dev_err(dev, "CDU write wait-idle failed\n");
			return ret;
		}

		remaining -= size;
		pos += size;
	}

	/*
	 * TODO: Check if we really need to send 48 zero bytes of data like
	 * downstream does.
	 */
	memset(&cdu, 0, sizeof(cdu));
	cdu.command = cpu_to_le16(AXIOM_CDU_CMD_COMMIT);
	cdu.parameters[0] = cpu_to_le16(AXIOM_CDU_PARAM0_COMMIT);
	cdu.parameters[1] = cpu_to_le16(AXIOM_CDU_PARAM1_COMMIT);

	ret = regmap_raw_write(ts->regmap, reg, &cdu, sizeof(cdu));
	if (ret) {
		dev_err(dev, "Failed to commit CDU u%x to NVM\n",
			chunk->usage_num);
		return ret;
	}

	ret = axiom_wait_for_completion_timeout(ts, &ts->nvm_write,
					msecs_to_jiffies(5 * MSEC_PER_SEC));
	if (!ret) {
		dev_err(ts->dev, "Error CDU u%x commit timedout\n",
			chunk->usage_num);
		return -ETIMEDOUT;
	}

	return axiom_cdu_wait_idle(ts, chunk->usage_num);
}

static int
axiom_write_cfg_chunk(struct axiom_data *ts, const struct axiom_fw_cfg_chunk *chunk)
{
	unsigned int reg;
	int ret;

	if (ts->usage_table[chunk->usage_num].info &&
	    ts->usage_table[chunk->usage_num].info->is_cdu) {
		ret = axiom_write_cdu_usage(ts, chunk);
		if (ret)
			return ret;
		goto out;
	}

	reg = axiom_usage_baseaddr(ts, chunk->usage_num);
	ret = regmap_raw_write(ts->regmap, reg, chunk->usage_content, chunk->usage_length);
	if (ret)
		return ret;

out:
	return axiom_u02_wait_idle(ts);
}

static int axiom_verify_volatile_mem(struct axiom_data *ts)
{
	int ret;

	ret = axiom_u02_computecrc(ts);
	if (ret)
		return ret;

	/* Query the new CRCs after they are re-computed */
	ret = axiom_u33_read(ts, &ts->crc[AXIOM_CRC_CUR]);
	if (ret)
		return ret;

	return ts->crc[AXIOM_CRC_CUR].vltusageconfig ==
	       ts->crc[AXIOM_CRC_NEW].vltusageconfig ? 0 : -EINVAL;
}

static int axiom_verify_crcs(struct axiom_data *ts)
{
	struct device *dev = ts->dev;
	struct axiom_crc *cur, *new;

	cur = &ts->crc[AXIOM_CRC_CUR];
	new = &ts->crc[AXIOM_CRC_NEW];

	if (new->vltusageconfig != cur->vltusageconfig) {
		dev_err(dev, "VLTUSAGECONFIG CRC32 mismatch (dev:%#x != fw:%#x)\n",
			cur->vltusageconfig, new->vltusageconfig);
		return -EINVAL;
	} else if (new->nvltlusageconfig != cur->nvltlusageconfig) {
		dev_err(dev, "NVLTUSAGECONFIG CRC32 mismatch (dev:%#x != fw:%#x)\n",
			cur->nvltlusageconfig, new->nvltlusageconfig);
		return -EINVAL;
	} else if (new->u22_sequencedata != cur->u22_sequencedata) {
		dev_err(dev, "U22_SEQUENCEDATA CRC32 mismatch (dev:%#x != fw:%#x)\n",
			cur->u22_sequencedata, new->u22_sequencedata);
		return -EINVAL;
	} else if (new->u43_hotspots != cur->u43_hotspots) {
		dev_err(dev, "U43_HOTSPOTS CRC32 mismatch (dev:%#x != fw:%#x)\n",
			cur->u43_hotspots, new->u43_hotspots);
		return -EINVAL;
	} else if (new->u93_profiles != cur->u93_profiles) {
		dev_err(dev, "U93_PROFILES CRC32 mismatch (dev:%#x != fw:%#x)\n",
			cur->u93_profiles, new->u93_profiles);
		return -EINVAL;
	} else if (new->u94_deltascalemap != cur->u94_deltascalemap) {
		dev_err(dev, "U94_DELTASCALEMAP CRC32 mismatch (dev:%#x != fw:%#x)\n",
			cur->u94_deltascalemap, new->u94_deltascalemap);
		return -EINVAL;
	}

	return 0;
}

static enum fw_upload_err
axiom_cfg_fw_write(struct fw_upload *fw_upload, const u8 *data, u32 offset,
		   u32 size, u32 *written)
{
	struct axiom_data *ts = fw_upload->dd_handle;
	struct axiom_firmware *afw = &ts->fw[AXIOM_FW_CFG];
	struct device *dev = ts->dev;
	bool cancel;
	int ret;

	/* Done before cancel check due to cleanup based put */
	ret = pm_runtime_resume_and_get(ts->dev);
	if (ret)
		return FW_UPLOAD_ERR_HW_ERROR;

	mutex_lock(&afw->lock);
	cancel = afw->cancel;
	mutex_unlock(&afw->lock);

	if (cancel)
		return FW_UPLOAD_ERR_CANCELED;

	axiom_lock_input_device(ts);

	if (ts->input && input_device_enabled(ts->input)) {
		dev_err(dev, "Input device not idle, abort TH2CFG update\n");
		axiom_unlock_input_device(ts);
		return FW_UPLOAD_ERR_HW_ERROR;
	}

	ret = axiom_u02_stop(ts);
	if (ret)
		goto err_swreset;

	ret = axiom_zero_volatile_mem(ts);
	if (ret)
		goto err_swreset;

	/* Skip to the first fw chunk */
	data += sizeof(struct axiom_fw_cfg_hdr);
	size -= sizeof(struct axiom_fw_cfg_hdr);
	*written += sizeof(struct axiom_fw_cfg_hdr);

	axiom_set_runmode(ts, AXIOM_TCP_CFG_UPDATE_MODE);

	while (size) {
		struct axiom_fw_cfg_chunk chunk;
		unsigned int chunk_len;

		chunk_len = axiom_cfg_fw_prepare_chunk(&chunk, data);
		if (axiom_skip_cfg_chunk(ts, &chunk)) {
			dev_dbg(dev, "Skip TH2CFG usage u%x\n", chunk.usage_num);
			goto next_chunk;
		}

		ret = axiom_write_cfg_chunk(ts, &chunk);
		if (ret) {
			axiom_set_runmode(ts, AXIOM_TCP_MODE);
			goto err_swreset;
		}

next_chunk:
		data += chunk_len;
		size -= chunk_len;
		*written += chunk_len;
	}

	axiom_set_runmode(ts, AXIOM_TCP_MODE);

	/* Ensure that the chunks are written correctly */
	ret = axiom_verify_volatile_mem(ts);
	if (ret) {
		dev_err(dev, "Failed to verify written config, abort\n");
		goto err_swreset;
	}

	ret = axiom_u02_save_config(ts);
	if (ret)
		goto err_swreset;

	/*
	 * TODO: Check if u02 start would be sufficient to load the new config
	 * values
	 */
	ret = axiom_u02_swreset(ts);
	if (ret) {
		dev_err(dev, "Soft reset failed\n");
		goto err_unlock;
	}

	ret = axiom_u33_read(ts, &ts->crc[AXIOM_CRC_CUR]);
	if (ret)
		goto err_unlock;

	if (axiom_verify_crcs(ts))
		goto err_unlock;

	/* Unlock before the input device gets unregistered */
	axiom_unlock_input_device(ts);

	ret = axiom_update_input_dev(ts);
	if (ret) {
		dev_err(dev, "Input device update failed after TH2CFG firmware update\n");
		goto err_out;
	}

	dev_info(dev, "TH2CFG update successful\n");

	return FW_UPLOAD_ERR_NONE;

err_swreset:
	axiom_u02_swreset(ts);
err_unlock:
	axiom_unlock_input_device(ts);
err_out:
	return ret == -ETIMEDOUT ? FW_UPLOAD_ERR_TIMEOUT : FW_UPLOAD_ERR_HW_ERROR;
}

static void axiom_cfg_fw_cancel(struct fw_upload *fw_upload)
{
	struct axiom_data *ts = fw_upload->dd_handle;
	struct axiom_firmware *afw = &ts->fw[AXIOM_FW_CFG];

	mutex_lock(&afw->lock);
	afw->cancel = true;
	mutex_unlock(&afw->lock);
}

static void axiom_cfg_fw_cleanup(struct fw_upload *fw_upload)
{
	struct axiom_data *ts = fw_upload->dd_handle;

	mutex_unlock(&ts->fwupdate_lock);
	pm_runtime_mark_last_busy(ts->dev);
	pm_runtime_put_sync_autosuspend(ts->dev);
}

static const struct fw_upload_ops axiom_cfg_fw_upload_ops = {
	.prepare = axiom_cfg_fw_prepare,
	.write = axiom_cfg_fw_write,
	.poll_complete = axiom_fw_poll_complete,
	.cancel = axiom_cfg_fw_cancel,
	.cleanup = axiom_cfg_fw_cleanup,
};

static void axiom_remove_axfw_fwl_action(void *data)
{
	struct axiom_data *ts = data;

	firmware_upload_unregister(ts->fw[AXIOM_FW_AXFW].fwl);
}

static void axiom_remove_cfg_fwl_action(void *data)
{
	struct axiom_data *ts = data;

	firmware_upload_unregister(ts->fw[AXIOM_FW_CFG].fwl);
}

static int axiom_register_fwl(struct axiom_data *ts)
{
	struct device *dev = ts->dev;
	struct fw_upload *fwl;
	char *fw_name;
	int ret;

	if (!IS_ENABLED(CONFIG_FW_UPLOAD)) {
		dev_dbg(dev, "axfw and th2cfgbin update disabled\n");
		return 0;
	}

	mutex_init(&ts->fw[AXIOM_FW_AXFW].lock);
	fw_name = kasprintf(GFP_KERNEL, "i2c:%s.axfw", dev_name(dev));
	fwl = firmware_upload_register(THIS_MODULE, ts->dev, fw_name,
				       &axiom_axfw_fw_upload_ops, ts);
	kfree(fw_name);
	if (IS_ERR(fwl))
		return dev_err_probe(dev, PTR_ERR(fwl),
				     "Failed to register firmware upload\n");

	ret = devm_add_action_or_reset(dev, axiom_remove_axfw_fwl_action, ts);
	if (ret)
		return ret;

	ts->fw[AXIOM_FW_AXFW].fwl = fwl;

	mutex_init(&ts->fw[AXIOM_FW_CFG].lock);
	fw_name = kasprintf(GFP_KERNEL, "i2c:%s.th2cfgbin", dev_name(dev));
	fwl = firmware_upload_register(THIS_MODULE, ts->dev, fw_name,
				       &axiom_cfg_fw_upload_ops, ts);
	kfree(fw_name);
	if (IS_ERR(fwl))
		return dev_err_probe(dev, PTR_ERR(fwl),
				     "Failed to register cfg firmware upload\n");

	ret = devm_add_action_or_reset(dev, axiom_remove_cfg_fwl_action, ts);
	if (ret)
		return ret;

	ts->fw[AXIOM_FW_CFG].fwl = fwl;

	return 0;
}

/************************* Device handlig *************************************/

#define AXIOM_SIMPLE_FW_DEVICE_ATTR(attr)					\
	static ssize_t								\
	fw_ ## attr ## _show(struct device *dev,				\
			     struct device_attribute *_attr, char *buf)		\
	{									\
		struct i2c_client *i2c = to_i2c_client(dev);			\
		struct axiom_data *ts = i2c_get_clientdata(i2c);		\
										\
		return sprintf(buf, "%u\n", ts->fw_##attr);			\
	}									\
	static DEVICE_ATTR_RO(fw_##attr)

AXIOM_SIMPLE_FW_DEVICE_ATTR(major);
AXIOM_SIMPLE_FW_DEVICE_ATTR(minor);
AXIOM_SIMPLE_FW_DEVICE_ATTR(rc);

static ssize_t fw_status_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct axiom_data *ts = i2c_get_clientdata(i2c);
	const char *val;

	if (ts->fw_status)
		val = "production";
	else
		val = "engineering";

	return sprintf(buf, "%s\n", val);
}
static DEVICE_ATTR_RO(fw_status);

static ssize_t device_id_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct axiom_data *ts = i2c_get_clientdata(i2c);

	return sprintf(buf, "%u\n", ts->device_id);
}
static DEVICE_ATTR_RO(device_id);

static ssize_t device_state_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct axiom_data *ts = i2c_get_clientdata(i2c);

	return sprintf(buf, "%s\n", axiom_runmode_to_string(ts));
}
static DEVICE_ATTR_RO(device_state);

static struct attribute *axiom_attrs[] = {
	&dev_attr_fw_major.attr,
	&dev_attr_fw_minor.attr,
	&dev_attr_fw_rc.attr,
	&dev_attr_fw_status.attr,
	&dev_attr_device_id.attr,
	&dev_attr_device_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(axiom);

static void axiom_poll(struct input_dev *input)
{
	struct axiom_data *ts = input_get_drvdata(input);

	axiom_process_report(ts, AXIOM_U34, NULL, 0);
}

static irqreturn_t axiom_irq(int irq, void *dev_id)
{
	struct axiom_data *ts = dev_id;

	axiom_process_report(ts, AXIOM_U34, NULL, 0);

	return IRQ_HANDLED;
}

static int axiom_input_open(struct input_dev *dev)
{
	struct axiom_data *ts = input_get_drvdata(dev);

	return pm_runtime_resume_and_get(ts->dev);
}

static void axiom_input_close(struct input_dev *dev)
{
	struct axiom_data *ts = input_get_drvdata(dev);

	pm_runtime_mark_last_busy(ts->dev);
	pm_runtime_put_sync_autosuspend(ts->dev);
}

static int axiom_register_input_dev(struct axiom_data *ts)
{
	struct device *dev = ts->dev;
	struct i2c_client *client = to_i2c_client(dev);
	struct input_dev *input;
	int ret;

	input = input_allocate_device();
	if (!input) {
		dev_err(dev, "Failed to allocate input driver data\n");
		return -ENOMEM;
	}

	input->dev.parent = dev;
	input->name = "TouchNetix aXiom Touchscreen";
	input->id.bustype = BUS_I2C;
	input->id.vendor = ts->jedec_id;
	input->id.product = ts->device_id;
	input->id.version = ts->silicon_rev;
	input->open = axiom_input_open;
	input->close = axiom_input_close;

	axiom_u64_cds_enabled(ts);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, AXIOM_MAX_XY - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, AXIOM_MAX_XY - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_DISTANCE, 0, 127, 0, 0);
	if (ts->cds_enabled)
		input_set_abs_params(input, ABS_MT_PRESSURE, 0, 127, 0, 0);

	touchscreen_parse_properties(input, true, &ts->prop);

	axiom_u42_get_touchslots(ts);
	if (!ts->num_slots) {
		input_free_device(input);
		dev_err(dev, "Error firmware has no touchslots enabled\n");
		return -EINVAL;
	}

	ret = input_mt_init_slots(input, ts->num_slots, INPUT_MT_DIRECT);
	if (ret) {
		input_free_device(input);
		dev_err(dev, "Failed to init mt slots\n");
		return ret;
	}

	/*
	 * Ensure that the IRQ setup is done only once since the handler belong
	 * to the i2c-dev whereas the input-poller belong to the input-dev. The
	 * input-dev can get unregistered during a firmware update to reflect
	 * the new firmware state. Therefore the input-poller setup must be done
	 * always.
	 */
	if (!ts->irq_setup_done && client->irq) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL, axiom_irq,
						IRQF_ONESHOT, dev_name(dev), ts);
		if (ret) {
			dev_err(dev, "Failed to request IRQ\n");
			return ret;
		}
		ts->irq_setup_done = true;
	} else {
		ret = input_setup_polling(input, axiom_poll);
		if (ret) {
			input_free_device(input);
			dev_err(dev, "Setup polling mode failed\n");
			return ret;
		}

		input_set_poll_interval(input, ts->poll_interval);
	}

	input_set_drvdata(input, ts);
	ts->input = input;

	ret = input_register_device(input);
	if (ret) {
		input_free_device(input);
		ts->input = NULL;
		dev_err(dev, "Failed to register input device\n");
	};

	return ret;
}

static int axiom_update_input_dev(struct axiom_data *ts)
{
	axiom_unregister_input_dev(ts);

	return axiom_register_input_dev(ts);
}

static int axiom_parse_firmware(struct axiom_data *ts)
{
	struct device *dev = ts->dev;
	struct gpio_desc *gpio;
	int ret;

	ts->supplies[0].supply = "vddi";
	ts->supplies[1].supply = "vdda";
	ts->num_supplies = ARRAY_SIZE(ts->supplies);

	ret = devm_regulator_bulk_get(dev, ts->num_supplies, ts->supplies);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get power supplies\n");

	gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(gpio))
		return dev_err_probe(dev, PTR_ERR(gpio),
				     "Failed to get reset GPIO\n");
	ts->reset_gpio = gpio;

	ts->poll_interval = AXIOM_DEFAULT_POLL_INTERVAL_MS;
	device_property_read_u32(dev, "poll-interval", &ts->poll_interval);

	return 0;
}

static int axiom_power_device(struct axiom_data *ts, unsigned int enable)
{
	struct device *dev = ts->dev;
	int ret;

	if (!enable) {
		regulator_bulk_disable(ts->num_supplies, ts->supplies);
		return 0;
	}

	ret = regulator_bulk_enable(ts->num_supplies, ts->supplies);
	if (ret) {
		dev_err(dev, "Failed to enable power supplies\n");
		return ret;
	}

	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	fsleep(2000);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);

	fsleep(AXIOM_STARTUP_TIME_MS);

	return 0;
}

static int axiom_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct axiom_data *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate driver data\n");

	ts->regmap = devm_regmap_init_i2c(client, &axiom_i2c_regmap_config);
	if (IS_ERR(ts->regmap))
		return dev_err_probe(dev, PTR_ERR(ts->regmap),
				     "Failed to initialize regmap\n");

	i2c_set_clientdata(client, ts);
	ts->dev = dev;

	init_completion(&ts->boot_complete.completion);
	init_completion(&ts->nvm_write.completion);
	mutex_init(&ts->fwupdate_lock);

	ret = axiom_register_fwl(ts);
	if (ret)
		return ret;

	ret = axiom_parse_firmware(ts);
	if (ret)
		return ret;

	ret = axiom_power_device(ts, 1);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to power-on device\n");

	pm_runtime_set_autosuspend_delay(dev, 10 * MSEC_PER_SEC);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable pm-runtime\n");

	ret = axiom_u31_device_discover(ts);
	/*
	 * Register the device to allow FW updates in case that the current FW
	 * doesn't support the required driver usages or if the device is in
	 * bootloader mode.
	 */
	if (ret && ret == -EACCES && IS_ENABLED(CONFIG_FW_UPLOAD)) {
		dev_warn(dev, "Device discovery failed, wait for user fw update\n");
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_sync_autosuspend(dev);
		return 0;
	} else if (ret) {
		pm_runtime_put_sync(dev);
		return dev_err_probe(dev, ret, "Device discovery failed\n");
	}

	ret = axiom_register_input_dev(ts);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_sync_autosuspend(dev);
	if (ret && IS_ENABLED(CONFIG_FW_UPLOAD))
		dev_warn(dev, "Failed to register the input device, wait for user fw update\n");
	else if (ret)
		return dev_err_probe(dev, ret, "Failed to register input device\n");

	return 0;
}

static void axiom_i2c_remove(struct i2c_client *client)
{
	struct axiom_data *ts = i2c_get_clientdata(client);

	axiom_unregister_input_dev(ts);
}

static int axiom_runtime_suspend(struct device *dev)
{
	struct axiom_data *ts = dev_get_drvdata(dev);
	struct i2c_client *client = to_i2c_client(dev);

	if (client->irq && ts->irq_setup_done)
		disable_irq(client->irq);

	return axiom_power_device(ts, 0);
}

static int axiom_runtime_resume(struct device *dev)
{
	struct axiom_data *ts = dev_get_drvdata(dev);
	struct i2c_client *client = to_i2c_client(dev);
	int ret;

	ret = axiom_power_device(ts, 1);
	if (ret)
		return ret;

	if (client->irq && ts->irq_setup_done)
		enable_irq(client->irq);

	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(axiom_pm_ops, axiom_runtime_suspend,
				 axiom_runtime_resume, NULL);

static const struct i2c_device_id axiom_i2c_id_table[] = {
	{ "ax54a" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, axiom_i2c_id_table);

static const struct of_device_id axiom_of_match[] = {
	{ .compatible = "touchnetix,ax54a", },
	{ }
};
MODULE_DEVICE_TABLE(of, axiom_of_match);

static struct i2c_driver axiom_i2c_driver = {
	.driver = {
		   .name = KBUILD_MODNAME,
		   .dev_groups = axiom_groups,
		   .pm = pm_ptr(&axiom_pm_ops),
		   .of_match_table = axiom_of_match,
	},
	.id_table = axiom_i2c_id_table,
	.probe = axiom_i2c_probe,
	.remove = axiom_i2c_remove,
};
module_i2c_driver(axiom_i2c_driver);

MODULE_DESCRIPTION("TouchNetix aXiom touchscreen I2C bus driver");
MODULE_LICENSE("GPL");
