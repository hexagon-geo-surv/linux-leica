// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Himax HM1246 image sensor
 *
 * Copyright 2025 Matthias Fend <matthias.fend@emfend.at>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>

/* Status registers */
#define HM1246_MODEL_ID_REG		 CCI_REG16(0x0000)

/* General setup registers */
#define HM1246_MODE_SELECT_REG		 CCI_REG8(0x0100)
#define HM1246_MODE_SELECT_STANDBY	 0x00
#define HM1246_MODE_SELECT_STREAM	 0x01
#define HM1246_MODE_SELECT_STOP		 0x02
#define HM1246_IMAGE_ORIENTATION_REG	 CCI_REG8(0x0101)
#define HM1246_IMAGE_ORIENTATION_VFLIP	 BIT(1)
#define HM1246_IMAGE_ORIENTATION_HFLIP	 BIT(0)
#define HM1246_CMU_UPDATE_REG		 CCI_REG8(0x0104)

/* Output setup registers */
#define HM1246_COARSE_INTG_REG		 CCI_REG16(0x0202)
#define HM1246_ANALOG_GLOBAL_GAIN_REG	 CCI_REG8(0x0205)

/* Clock setup registers */
#define HM1246_PLL1CFG_REG		 CCI_REG8(0x0303)
#define HM1246_PLL1CFG_MULTIPLIER(x)	 (((x) & 0xFF) << 0)
#define HM1246_PLL2CFG_REG		 CCI_REG8(0x0305)
#define HM1246_PLL2CFG_PRE_DIV(x)	 (((x) & 0x1F) << 1)
#define HM1246_PLL2CFG_MULTIPLIER(x)	 (((x) & 0x01) << 0)
#define HM1246_PLL3CFG_REG		 CCI_REG8(0x0307)
#define HM1246_PLL3CFG_POST_DIV(x)	 (((x) & 0x3) << 6)
#define HM1246_PLL3CFG_SYSCLK_DIV(x)	 (((x) & 0x3) << 4)
#define HM1246_PLL3CFG_PCLK_DIV(x)	 (((x) & 0x7) << 0)

/* Frame timing registers */
#define HM1246_FRAME_LENGTH_LINES_REG	 CCI_REG16(0x0340)
#define HM1246_LINE_LENGTH_PCK_REG	 CCI_REG16(0x0342)

/* Image size registers */
#define HM1246_X_ADDR_START_REG		 CCI_REG16(0x0344)
#define HM1246_Y_ADDR_START_REG		 CCI_REG16(0x0346)
#define HM1246_X_ADDR_END_REG		 CCI_REG16(0x0348)
#define HM1246_Y_ADDR_END_REG		 CCI_REG16(0x034A)
#define HM1246_X_LA_START_REG		 CCI_REG16(0x0351)
#define HM1246_X_LA_END_REG		 CCI_REG16(0x0353)
#define HM1246_Y_LA_START_REG		 CCI_REG16(0x0355)
#define HM1246_Y_LA_END_REG		 CCI_REG16(0x0357)

/* Test pattern registers */
#define HM1246_TEST_PATTERN_MODE_REG	 CCI_REG8(0x0601)
#define HM1246_TEST_PATTERN_MODE_MODE(x) (((x) & 0xF) << 4)
#define HM1246_TEST_PATTERN_MODE_ENABLE	 BIT(0)
#define HM1246_TEST_DATA_BLUE_REG	 CCI_REG16(0x0602)
#define HM1246_TEST_DATA_GB_REG		 CCI_REG16(0x0604)
#define HM1246_TEST_DATA_RED_REG	 CCI_REG16(0x0606)
#define HM1246_TEST_DATA_GR_REG		 CCI_REG16(0x0608)

/* SBC registers */
#define HM1246_SBC_BOOT_REF2_REG	 CCI_REG8(0x2001)
#define HM1246_SBC_BOOT_REF2_PLL_LOCK	 BIT(4)
#define HM1246_SBC_CTRL_REG		 CCI_REG8(0x2003)
#define HM1246_SBC_CTRL_PLL_EN		 BIT(0)

/* System registers */
#define HM1246_OUTPUT_PRT_CTRL_REG	 CCI_REG8(0x2F02)
#define HM1246_POLARITY_CTRL_REG	 CCI_REG8(0x2F20)
#define HM1246_POLARITY_CTRL_HSYNC	 BIT(7)
#define HM1246_POLARITY_CTRL_VSYNC	 BIT(6)
#define HM1246_PCLK_CTRL_REG		 CCI_REG8(0x2F24)
#define HM1246_PCLK_CTRL_POL		 BIT(3)

/* Digital window control & parameter registers */
#define HM1246_DWIN_XOFFSET_REG		 CCI_REG16(0xD5E4)
#define HM1246_DWIN_XSIZE_REG		 CCI_REG16(0xD5E6)
#define HM1246_DWIN_YOFFSET_REG		 CCI_REG16(0xD5E8)
#define HM1246_DWIN_YSIZE_REG		 CCI_REG16(0xD5EA)

#define HM1246_MODEL_ID			 0x1245

#define HM1246_NATIVE_WIDTH		 1296
#define HM1246_NATIVE_HEIGHT		 976

#define HM1246_VTS_MAX			 65535

#define HM1246_COARSE_INTG_MARGIN	 2
#define HM1246_COARSE_INTG_MIN		 4
#define HM1246_COARSE_INTG_STEP		 1

#define HM1246_ANALOG_GLOBAL_GAIN_MIN	 0x00
#define HM1246_ANALOG_GLOBAL_GAIN_MAX	 0xE8
#define HM1246_ANALOG_GLOBAL_GAIN_STEP	 1

#define HM1246_XCLK_MIN			 (6 * HZ_PER_MHZ)
#define HM1246_XCLK_MAX			 (27 * HZ_PER_MHZ)

#define HM1246_PCLK_MIN			 (8 * HZ_PER_MHZ)
#define HM1246_PCLK_MAX			 (96 * HZ_PER_MHZ)

#define HM1246_PLL_VCO_MIN		 360000000
#define HM1246_PLL_VCO_MAX		 680000000

#define HM1246_PLL_INCLK_MIN		 1000000
#define HM1246_PLL_INCLK_MAX		 2500000

#define HM1246_PLL_MULTI_L_MIN		 1
#define HM1246_PLL_MULTI_L_MAX		 256

#define HM1246_PLL_MULTI_H_MIN		 2
#define HM1246_PLL_MULTI_H_MAX		 3

#define HM1246_PLL_MULTI_MIN \
	(HM1246_PLL_MULTI_H_MIN * HM1246_PLL_MULTI_L_MIN)
#define HM1246_PLL_MULTI_MAX \
	(HM1246_PLL_MULTI_H_MAX * HM1246_PLL_MULTI_L_MAX)

static const char *const hm1246_test_pattern_menu[] = {
	"Disabled",
	"Checkboard",
	"Ramp",
	"Moving ones",
	"Blending color bars",
	"Color bars",
	"Solid white",
	"Solid black",
	"Solid red",
	"Solid green",
	"Solid blue",
};

static const s64 hm1246_link_freqs[] = {
	42174000, /* 1420x990 @ 30Hz (RAW) */
};

static const char *const hm1246_supply_names[] = {
	"avdd",
	"iovdd",
	"dvdd",
};

struct hm1246 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct regulator_bulk_data supplies[ARRAY_SIZE(hm1246_supply_names)];
	struct clk *xclk;
	unsigned long xclk_freq;
	struct gpio_desc *reset_gpio;
	unsigned int mbus_flags;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate_ctrl;
	struct v4l2_ctrl *link_freq_ctrl;
	struct v4l2_ctrl *exposure_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
	struct v4l2_ctrl *hblank_ctrl;
	struct v4l2_ctrl *hflip_ctrl;
	struct v4l2_ctrl *vflip_ctrl;

	struct regmap *regmap;

	bool identified;
	const struct hm1246_mode *cur_mode;
};

static const struct cci_reg_sequence mode_1296x976_raw[] = {
	{ HM1246_X_LA_START_REG, 60 },
	{ HM1246_X_LA_END_REG, 1355 },
	{ HM1246_Y_LA_START_REG, 0 },
	{ HM1246_Y_LA_END_REG, 975 },
	{ HM1246_OUTPUT_PRT_CTRL_REG, 0x20 },
	{ CCI_REG8(0x300A), 0x01 },
	{ CCI_REG8(0x300B), 0x00 },
	{ CCI_REG8(0x50F5), 0x01 },
	{ CCI_REG8(0x50DD), 0x00 },
	{ CCI_REG8(0x50A1), 0x02 },
	{ CCI_REG8(0x50AA), 0x1C },
	{ CCI_REG8(0x50AC), 0xDD },
	{ CCI_REG8(0x50AD), 0x08 },
	{ CCI_REG8(0x50AB), 0x04 },
	{ CCI_REG8(0x50A0), 0x40 },
	{ CCI_REG8(0x50A2), 0x12 },
	{ CCI_REG8(0x50AE), 0x30 },
	{ CCI_REG8(0x50B3), 0x04 },
	{ CCI_REG8(0x5200), 0x04 },
	{ CCI_REG8(0x5204), 0x40 },
	{ CCI_REG8(0x5208), 0x55 },
	{ CCI_REG8(0x5209), 0x06 },
	{ CCI_REG8(0x520B), 0x05 },
	{ CCI_REG8(0x520D), 0x40 },
	{ CCI_REG8(0x5214), 0x18 },
	{ CCI_REG8(0x5215), 0x0F },
	{ CCI_REG8(0x5217), 0x01 },
	{ CCI_REG8(0x5218), 0x07 },
	{ CCI_REG8(0x5219), 0x01 },
	{ CCI_REG8(0x521A), 0x50 },
	{ CCI_REG8(0x521B), 0x24 },
	{ CCI_REG8(0x5232), 0x01 },
	{ CCI_REG8(0x5220), 0x11 },
	{ CCI_REG8(0x5227), 0x01 },
	{ CCI_REG8(0x5106), 0xC1 },
	{ CCI_REG8(0x5115), 0xC0 },
	{ CCI_REG8(0x5116), 0xC1 },
	{ CCI_REG8(0x5138), 0x40 },
	{ CCI_REG8(0x5139), 0x60 },
	{ CCI_REG8(0x513A), 0x80 },
	{ CCI_REG8(0x513B), 0xA0 },
	{ CCI_REG8(0x513C), 0xA1 },
	{ CCI_REG8(0x513D), 0xA2 },
	{ CCI_REG8(0x513E), 0xA3 },
	{ CCI_REG8(0x5140), 0x40 },
	{ CCI_REG8(0x5141), 0x60 },
	{ CCI_REG8(0x5142), 0x80 },
	{ CCI_REG8(0x5143), 0x81 },
	{ CCI_REG8(0x5144), 0x82 },
	{ CCI_REG8(0x5145), 0x83 },
	{ CCI_REG8(0x5146), 0x93 },
	{ CCI_REG8(0x51C1), 0xC3 },
	{ CCI_REG8(0x51C5), 0xC3 },
	{ CCI_REG8(0x51C9), 0xC3 },
	{ CCI_REG8(0x51CD), 0xC2 },
	{ CCI_REG8(0x51D1), 0xC1 },
	{ CCI_REG8(0x51D5), 0xC1 },
	{ CCI_REG8(0x51D9), 0x81 },
	{ CCI_REG8(0x51DD), 0x81 },
	{ CCI_REG8(0x51C2), 0x49 },
	{ CCI_REG8(0x51C6), 0x49 },
	{ CCI_REG8(0x51CA), 0x49 },
	{ CCI_REG8(0x51CE), 0x49 },
	{ CCI_REG8(0x51D2), 0x49 },
	{ CCI_REG8(0x51D6), 0x59 },
	{ CCI_REG8(0x51DA), 0x59 },
	{ CCI_REG8(0x51DE), 0x59 },
	{ CCI_REG8(0x51C3), 0x20 },
	{ CCI_REG8(0x51C7), 0x38 },
	{ CCI_REG8(0x51CB), 0x21 },
	{ CCI_REG8(0x51CF), 0x11 },
	{ CCI_REG8(0x51D3), 0x11 },
	{ CCI_REG8(0x51D7), 0x13 },
	{ CCI_REG8(0x51DB), 0x13 },
	{ CCI_REG8(0x51DF), 0x13 },
	{ CCI_REG8(0x51E0), 0x03 },
	{ CCI_REG8(0x51E2), 0x03 },
	{ CCI_REG8(0x51F0), 0x42 },
	{ CCI_REG8(0x51F1), 0x40 },
	{ CCI_REG8(0x51F2), 0x4A },
	{ CCI_REG8(0x51F3), 0x48 },
	{ CCI_REG8(0x5015), 0x73 },
	{ CCI_REG8(0x504A), 0x04 },
	{ CCI_REG8(0x5044), 0x07 },
	{ CCI_REG8(0x5040), 0x03 },
	{ CCI_REG8(0x5135), 0xC4 },
	{ CCI_REG8(0x5136), 0xC5 },
	{ CCI_REG8(0x5166), 0xC4 },
	{ CCI_REG8(0x5196), 0xC4 },
	{ CCI_REG8(0x51C0), 0x10 },
	{ CCI_REG8(0x51C4), 0x10 },
	{ CCI_REG8(0x51C8), 0xA0 },
	{ CCI_REG8(0x51CC), 0xA0 },
	{ CCI_REG8(0x51D0), 0xA1 },
	{ CCI_REG8(0x51D4), 0xA5 },
	{ CCI_REG8(0x51D8), 0xA5 },
	{ CCI_REG8(0x51DC), 0xA5 },
	{ CCI_REG8(0x5200), 0xE4 },
	{ CCI_REG8(0x5209), 0x04 },
	{ CCI_REG8(0x301B), 0x01 },
	{ CCI_REG8(0x3130), 0x01 },
	{ CCI_REG8(0x5013), 0x07 },
	{ CCI_REG8(0x5016), 0x01 },
	{ CCI_REG8(0x501D), 0x50 },
	{ CCI_REG8(0x0350), 0xFE },
	{ CCI_REG8(0x0343), 0x8C },
	{ CCI_REG8(0x2F03), 0x15 },
	{ CCI_REG8(0xD380), 0x00 },
	{ CCI_REG8(0x3047), 0x7F },
	{ CCI_REG8(0x304D), 0x34 },
	{ CCI_REG8(0x3041), 0x4B },
	{ CCI_REG8(0x3042), 0x2D },
	{ CCI_REG8(0x3056), 0x64 },
	{ CCI_REG8(0x3059), 0x1E },
	{ CCI_REG8(0x305E), 0x10 },
	{ CCI_REG8(0x305F), 0x10 },
	{ CCI_REG8(0x306D), 0x10 },
	{ CCI_REG8(0x306E), 0x0C },
	{ CCI_REG8(0x3064), 0x50 },
	{ CCI_REG8(0x3067), 0x78 },
	{ CCI_REG8(0x3068), 0x4B },
	{ CCI_REG8(0x306A), 0x78 },
	{ CCI_REG8(0x306B), 0x4B },
	{ CCI_REG8(0xD442), 0x3D },
	{ CCI_REG8(0xD443), 0x06 },
	{ CCI_REG8(0xD440), 0x63 },
	{ CCI_REG8(0xD446), 0xB0 },
	{ CCI_REG8(0xD447), 0x60 },
	{ CCI_REG8(0xD448), 0x48 },
	{ CCI_REG8(0xD449), 0x30 },
	{ CCI_REG8(0xD44A), 0x18 },
	{ CCI_REG8(0xD360), 0x03 },
	{ CCI_REG8(0x30AC), 0x10 },
	{ CCI_REG8(0x30AD), 0x10 },
	{ CCI_REG8(0x30AE), 0x10 },
	{ CCI_REG8(0x3040), 0x0B },
	{ CCI_REG8(0x2002), 0x00 },
	{ CCI_REG8(0x2000), 0x08 },
};

struct hm1246_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

struct hm1246_mode {
	u32 codes[4];
	u32 link_freq_index;
	u32 clocks_per_pixel;
	u32 top;
	u32 left;
	u32 width;
	u32 height;
	u32 hts;
	u32 vts_min;
	const struct hm1246_reg_list reg_list;
};

#define FLIP_FORMAT_INDEX(v, h) ((v ? 2 : 0) | (h ? 1 : 0))

/* Get the format code of the mode considering current flip setting. */
static u32 hm1246_get_format_code(struct hm1246 *hm1246,
				  const struct hm1246_mode *hm1246_mode)
{
	return hm1246_mode->codes[FLIP_FORMAT_INDEX(hm1246->vflip_ctrl->val,
						    hm1246->hflip_ctrl->val)];
}

static const struct hm1246_mode hm1246_modes[] = {
	{
		.codes = {
			[FLIP_FORMAT_INDEX(0, 0)] = MEDIA_BUS_FMT_SBGGR10_1X10,
			[FLIP_FORMAT_INDEX(0, 1)] = MEDIA_BUS_FMT_SGBRG10_1X10,
			[FLIP_FORMAT_INDEX(1, 0)] = MEDIA_BUS_FMT_SGRBG10_1X10,
			[FLIP_FORMAT_INDEX(1, 1)] = MEDIA_BUS_FMT_SRGGB10_1X10,
		},
		.link_freq_index = 0,
		.clocks_per_pixel = 1,
		.top = 0,
		.left = 0,
		.width = 1296,
		.height = 976,
		.hts = 1420,
		.vts_min = 990,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1296x976_raw),
			.regs = mode_1296x976_raw,
		},
	},
};

static inline struct hm1246 *to_hm1246(struct v4l2_subdev *sd)
{
	return container_of(sd, struct hm1246, sd);
}

static const struct hm1246_mode *
hm1246_find_mode_by_mbus_code(struct hm1246 *hm1246, u32 code)
{
	for (int i = 0; i < ARRAY_SIZE(hm1246_modes); i++) {
		if (code == hm1246_get_format_code(hm1246, &hm1246_modes[i]))
			return &hm1246_modes[i];
	}

	return ERR_PTR(-EINVAL);
}

static int hm1246_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hm1246 *hm1246 = to_hm1246(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(hm1246_supply_names),
				    hm1246->supplies);
	if (ret) {
		dev_err(hm1246->dev, "failed to enable regulators\n");
		return ret;
	}

	ret = clk_prepare_enable(hm1246->xclk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(hm1246_supply_names),
				       hm1246->supplies);
		dev_err(hm1246->dev, "failed to enable clock\n");
		return ret;
	}

	gpiod_set_value_cansleep(hm1246->reset_gpio, 0);

	/*
	 * XSHUTDOWN to crystal clock oscillation:  tcrystal typ.  650us
	 * Sample bootstrap pin:                    tsample  max. 2000us
	 * Built in self test:                      tbist    max. 3000us
	 */
	fsleep(6000);

	return 0;
}

static int hm1246_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hm1246 *hm1246 = to_hm1246(sd);

	gpiod_set_value_cansleep(hm1246->reset_gpio, 1);

	clk_disable_unprepare(hm1246->xclk);

	regulator_bulk_disable(ARRAY_SIZE(hm1246_supply_names),
			       hm1246->supplies);

	return 0;
}

static int hm1246_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct hm1246 *hm1246 = to_hm1246(sd);

	if (code->index >= ARRAY_SIZE(hm1246_modes))
		return -EINVAL;

	code->code = hm1246_get_format_code(hm1246, &hm1246_modes[code->index]);

	return 0;
}

static int hm1246_enum_frame_size(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct hm1246 *hm1246 = to_hm1246(subdev);
	const struct hm1246_mode *mode = NULL;

	if (fse->index > 0)
		return -EINVAL;

	mode = hm1246_find_mode_by_mbus_code(hm1246, fse->code);
	if (IS_ERR(mode))
		return PTR_ERR(mode);

	fse->min_width = mode->width;
	fse->max_width = mode->width;
	fse->min_height = mode->height;
	fse->max_height = mode->height;

	return 0;
}

static int hm1246_update_controls(struct hm1246 *hm1246,
				  const struct hm1246_mode *mode)
{
	s64 pixel_rate, exposure_max, vblank, hblank;
	int ret;

	ret = __v4l2_ctrl_s_ctrl(hm1246->link_freq_ctrl, mode->link_freq_index);
	if (ret) {
		dev_err(hm1246->dev, "link_freq ctrl range update failed\n");
		return ret;
	}

	pixel_rate = div_u64(hm1246_link_freqs[mode->link_freq_index],
			     mode->clocks_per_pixel);
	ret = __v4l2_ctrl_modify_range(hm1246->pixel_rate_ctrl, pixel_rate,
				       pixel_rate, 1, pixel_rate);
	if (ret) {
		dev_err(hm1246->dev, "pixel_rate ctrl range update failed\n");
		return ret;
	}

	vblank = mode->vts_min - mode->height,
	ret = __v4l2_ctrl_modify_range(hm1246->vblank_ctrl, vblank,
				       HM1246_VTS_MAX - mode->height, 1,
				       vblank);
	if (ret) {
		dev_err(hm1246->dev, "vblank ctrl range update failed\n");
		return ret;
	}

	hblank = mode->hts - mode->width;
	ret = __v4l2_ctrl_modify_range(hm1246->hblank_ctrl, hblank, hblank, 1,
				       hblank);
	if (ret) {
		dev_err(hm1246->dev, "hblank ctrl range update failed\n");
		return ret;
	}

	exposure_max = mode->vts_min - HM1246_COARSE_INTG_MARGIN;
	ret = __v4l2_ctrl_modify_range(hm1246->exposure_ctrl,
				       HM1246_COARSE_INTG_MIN, exposure_max,
				       HM1246_COARSE_INTG_STEP, exposure_max);
	if (ret) {
		dev_err(hm1246->dev, "exposure ctrl range update failed\n");
		return ret;
	}

	return 0;
}

static void hm1246_update_pad_format(struct hm1246 *hm1246,
				     const struct hm1246_mode *hm1246_mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = hm1246_mode->width;
	fmt->height = hm1246_mode->height;
	fmt->code = hm1246_get_format_code(hm1246, hm1246_mode);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int hm1246_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct hm1246 *hm1246 = to_hm1246(sd);
	struct v4l2_mbus_framefmt *mbus_fmt;
	struct v4l2_rect *crop;
	const struct hm1246_mode *mode;

	mode = hm1246_find_mode_by_mbus_code(hm1246, fmt->format.code);
	if (IS_ERR(mode))
		mode = &hm1246_modes[0];

	crop = v4l2_subdev_state_get_crop(state, 0);
	crop->top = mode->top;
	crop->left = mode->left;
	crop->width = mode->width;
	crop->height = mode->height;

	hm1246_update_pad_format(hm1246, mode, &fmt->format);
	mbus_fmt = v4l2_subdev_state_get_format(state, 0);
	*mbus_fmt = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	hm1246->cur_mode = mode;
	hm1246_update_controls(hm1246, mode);

	return 0;
}

static int hm1246_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct hm1246 *hm1246 = to_hm1246(sd);
	const struct hm1246_mode *mode = hm1246->cur_mode;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = HM1246_NATIVE_WIDTH;
		sel->r.height = HM1246_NATIVE_HEIGHT;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = mode->top;
		sel->r.left = mode->left;
		sel->r.width = mode->width;
		sel->r.height = mode->height;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int hm1246_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct hm1246 *hm1246 = to_hm1246(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = hm1246_get_format_code(hm1246,
						       &hm1246_modes[0]),
			.width = hm1246_modes[0].width,
			.height = hm1246_modes[0].height,
		},
	};

	hm1246_set_format(sd, state, &fmt);

	return 0;
}

static int hm1246_calc_pll(struct hm1246 *hm1246, u32 xclk, u32 link_freq,
			   u32 clocks_per_pixel, u8 *pll1, u8 *pll2, u8 *pll3)
{
	const u8 pclk_div_table[] = { 4, 5, 6, 7, 8, 12, 14, 16 };
	const u8 sysclk_div_table[] = { 1, 2, 3, 4 };
	const u8 post_div_table[] = { 1, 2, 4, 8 };
	const int sysclk_pclk_ratio = 3; /* Recommended value */
	u32 pclk, vco_out, best_vco_diff;
	int pclk_div_index, sysclk_div_index, post_div_index;
	u8 pre_div, multiplier_h, multiplier_l;
	bool sysclk_pclk_ratio_found = false;

	if (link_freq < HM1246_PCLK_MIN || link_freq > HM1246_PCLK_MAX)
		return -EINVAL;

	/* In raw mode (1 pixel per clock) the pixel clock is internally
	 * divided by two.
	 */
	pclk = (2 * link_freq) / clocks_per_pixel;

	/* Find suitable PCLK and SYSCLK dividers. */
	for (pclk_div_index = 0; pclk_div_index < ARRAY_SIZE(pclk_div_table);
	     pclk_div_index++) {
		for (sysclk_div_index = 0;
		     sysclk_div_index < ARRAY_SIZE(sysclk_div_table);
		     sysclk_div_index++) {
			if (sysclk_div_table[sysclk_div_index] *
				    sysclk_pclk_ratio ==
			    pclk_div_table[pclk_div_index]) {
				sysclk_pclk_ratio_found = true;
				break;
			}
		}
		if (sysclk_pclk_ratio_found)
			break;
	}

	if (!sysclk_pclk_ratio_found)
		return -EINVAL;

	/* Determine an appropriate post divider. */
	for (post_div_index = 0; post_div_index < ARRAY_SIZE(post_div_table);
	     post_div_index++) {
		vco_out = pclk * (pclk_div_table[pclk_div_index] *
				  post_div_table[post_div_index]);

		if (vco_out >= HM1246_PLL_VCO_MIN &&
		    vco_out <= HM1246_PLL_VCO_MAX)
			break;
	}
	if (post_div_index >= ARRAY_SIZE(post_div_table))
		return -EINVAL;

	/* Find best pre-divider and multiplier values. */
	best_vco_diff = U32_MAX;
	for (u32 div = DIV_ROUND_UP(xclk, HM1246_PLL_INCLK_MAX);
	     div <= (xclk / HM1246_PLL_INCLK_MIN); div++) {
		u32 multi, multi_h, multi_l, vco, diff;

		multi = DIV_ROUND_CLOSEST_ULL((u64)vco_out * div, xclk);
		if (multi < HM1246_PLL_MULTI_MIN ||
		    multi > HM1246_PLL_MULTI_MAX)
			continue;

		multi_h = multi / (HM1246_PLL_MULTI_H_MIN *
				   HM1246_PLL_MULTI_L_MAX) +
			  2;
		multi_l = multi / multi_h;
		vco = div_u64((u64)xclk * multi_h * multi_l, div);

		diff = abs(vco_out - vco);
		if (diff < best_vco_diff) {
			best_vco_diff = diff;
			pre_div = div;
			multiplier_h = multi_h;
			multiplier_l = multi_l;
		}

		if (!diff)
			break;
	}

	if (best_vco_diff == U32_MAX)
		return -EINVAL;

	*pll1 = HM1246_PLL1CFG_MULTIPLIER(multiplier_l - 1);
	*pll2 = HM1246_PLL2CFG_PRE_DIV(pre_div - 1) |
		HM1246_PLL2CFG_MULTIPLIER(multiplier_h - 2);
	*pll3 = HM1246_PLL3CFG_POST_DIV(post_div_index) |
		HM1246_PLL3CFG_SYSCLK_DIV(sysclk_div_index) |
		HM1246_PLL3CFG_PCLK_DIV(pclk_div_index);

	return 0;
}

static int hm1246_cci_write_pll(struct hm1246 *hm1246, u8 pll1, u8 pll2,
				u8 pll3)
{
	struct cci_reg_sequence pll_regs[] = {
		{ HM1246_PLL1CFG_REG, pll1 },
		{ HM1246_PLL2CFG_REG, pll2 },
		{ HM1246_PLL3CFG_REG, pll3 },
		{ HM1246_SBC_CTRL_REG, HM1246_SBC_CTRL_PLL_EN },
	};

	return cci_multi_reg_write(hm1246->regmap, pll_regs,
				   ARRAY_SIZE(pll_regs), NULL);
}

static int hm1246_pll_check_locked(struct hm1246 *hm1246)
{
	u64 boot_ref2;
	int ret;

	ret = cci_read(hm1246->regmap, HM1246_SBC_BOOT_REF2_REG, &boot_ref2,
		       NULL);
	if (ret)
		return ret;

	return (boot_ref2 & HM1246_SBC_BOOT_REF2_PLL_LOCK) ? 0 : -EIO;
}

static int hm1246_setup_pll(struct hm1246 *hm1246,
			    const struct hm1246_mode *mode)
{
	u8 pll1, pll2, pll3;
	int ret;

	ret = hm1246_calc_pll(hm1246, hm1246->xclk_freq,
			      hm1246_link_freqs[mode->link_freq_index],
			      mode->clocks_per_pixel, &pll1, &pll2, &pll3);
	if (ret)
		return ret;

	ret = hm1246_cci_write_pll(hm1246, pll1, pll2, pll3);
	if (ret)
		return ret;

	/* PLL lock time: tpll typ. 100us */
	fsleep(200);

	ret = hm1246_pll_check_locked(hm1246);
	if (ret)
		return ret;

	return 0;
}

static int hm1246_cci_write_test_pattern(struct hm1246 *hm1246, u8 mode, u16 r,
					 u16 g, u16 b)
{
	struct cci_reg_sequence tpg_enable_regs[] = {
		{ HM1246_TEST_DATA_RED_REG, r },
		{ HM1246_TEST_DATA_GR_REG, g },
		{ HM1246_TEST_DATA_GB_REG, g },
		{ HM1246_TEST_DATA_BLUE_REG, b },
		{ HM1246_TEST_PATTERN_MODE_REG, mode },
	};

	return cci_multi_reg_write(hm1246->regmap, tpg_enable_regs,
				   ARRAY_SIZE(tpg_enable_regs), NULL);
}

static int hm1246_test_pattern(struct hm1246 *hm1246, u32 pattern_index)
{
	const u16 RGBMAX = 0x3FF;
	int pattern;
	u8 mode = HM1246_TEST_PATTERN_MODE_ENABLE;
	u16 r = 0, g = 0, b = 0;

	switch (pattern_index) {
	case 1: /* Checkboard Pattern */
		pattern = 0;
		break;
	case 2: /* Ramp */
		pattern = 1;
		break;
	case 3: /* Moving ones */
		pattern = 2;
		break;
	case 4: /* Blending color bars */
		pattern = 3;
		break;
	case 5: /* Color bars */
		pattern = 4;
		break;
	case 6: /* Solid white */
		pattern = 15;
		r = RGBMAX;
		g = RGBMAX;
		b = RGBMAX;
		break;
	case 7: /* Solid black */
		pattern = 15;
		break;
	case 8: /* Solid red */
		pattern = 15;
		r = RGBMAX;
		break;
	case 9: /* Solid green */
		pattern = 15;
		g = RGBMAX;
		break;
	case 10: /* Solid blue */
		pattern = 15;
		b = RGBMAX;
		break;

	case 0: /* Disabled */
	default:
		mode = 0;
		pattern = 0;
		break;
	}

	mode |= HM1246_TEST_PATTERN_MODE_MODE(pattern);

	return hm1246_cci_write_test_pattern(hm1246, mode, r, g, b);
}

static int hm1246_cci_write_cmu(struct hm1246 *hm1246, u32 reg, u64 val)
{
	struct cci_reg_sequence reg_seq[] = {
		{ reg, val },
		{ HM1246_CMU_UPDATE_REG, 0 },
	};

	return cci_multi_reg_write(hm1246->regmap, reg_seq, ARRAY_SIZE(reg_seq),
				   NULL);
}

static int hm1246_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hm1246 *hm1246 = container_of(ctrl->handler, struct hm1246,
					     ctrls);
	const struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	u32 val;
	int ret;

	state = v4l2_subdev_get_locked_active_state(&hm1246->sd);
	format = v4l2_subdev_state_get_format(state, 0);

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 exposure_max;

		exposure_max =
			format->height + ctrl->val - HM1246_COARSE_INTG_MARGIN;
		ret = __v4l2_ctrl_modify_range(hm1246->exposure_ctrl,
					       hm1246->exposure_ctrl->minimum,
					       exposure_max,
					       hm1246->exposure_ctrl->step,
					       exposure_max);

		if (ret) {
			dev_err(hm1246->dev, "exposure ctrl range update failed\n");
			return ret;
		}
	}

	if (!pm_runtime_get_if_active(hm1246->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = hm1246_cci_write_cmu(hm1246, HM1246_COARSE_INTG_REG,
					   ctrl->val);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		ret = hm1246_cci_write_cmu(hm1246,
					   HM1246_ANALOG_GLOBAL_GAIN_REG,
					   ctrl->val);
		break;

	case V4L2_CID_VBLANK:
		val = hm1246->cur_mode->height + ctrl->val;
		ret = hm1246_cci_write_cmu(hm1246,
					   HM1246_FRAME_LENGTH_LINES_REG, val);
		break;

	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		val = 0;
		if (hm1246->hflip_ctrl->val)
			val |= HM1246_IMAGE_ORIENTATION_HFLIP;
		if (hm1246->vflip_ctrl->val)
			val |= HM1246_IMAGE_ORIENTATION_VFLIP;

		ret = hm1246_cci_write_cmu(hm1246, HM1246_IMAGE_ORIENTATION_REG,
					   val);
		break;

	case V4L2_CID_TEST_PATTERN:
		ret = hm1246_test_pattern(hm1246, ctrl->val);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(hm1246->dev);

	return ret;
}

static const struct v4l2_ctrl_ops hm1246_ctrl_ops = {
	.s_ctrl = hm1246_set_ctrl,
};

static int hm1246_identify_module(struct hm1246 *hm1246)
{
	u64 model_id;
	int ret;

	if (hm1246->identified)
		return 0;

	ret = cci_read(hm1246->regmap, HM1246_MODEL_ID_REG, &model_id, NULL);
	if (ret)
		return ret;

	if (model_id != HM1246_MODEL_ID) {
		dev_err(hm1246->dev, "model id mismatch: 0x%llx!=0x%x\n",
			model_id, HM1246_MODEL_ID);
		return -ENXIO;
	}

	hm1246->identified = true;

	return 0;
}

static int hm1246_setup_moderegs(struct hm1246 *hm1246,
				 const struct hm1246_mode *mode)
{
	const struct hm1246_reg_list *reg_list = &mode->reg_list;
	const struct cci_reg_sequence modeaw[] = {
		{ HM1246_X_ADDR_START_REG, mode->left },
		{ HM1246_Y_ADDR_START_REG, mode->top },
		{ HM1246_X_ADDR_END_REG, mode->width - 1 },
		{ HM1246_Y_ADDR_END_REG, mode->height - 1 },
		{ HM1246_DWIN_XOFFSET_REG, mode->left },
		{ HM1246_DWIN_YOFFSET_REG, mode->top },
		{ HM1246_DWIN_XSIZE_REG, mode->width },
		{ HM1246_DWIN_YSIZE_REG, mode->height },
		{ HM1246_LINE_LENGTH_PCK_REG, mode->hts },
	};
	int ret = 0;

	cci_multi_reg_write(hm1246->regmap, modeaw, ARRAY_SIZE(modeaw), &ret);
	cci_multi_reg_write(hm1246->regmap, reg_list->regs,
			    reg_list->num_of_regs, &ret);

	return ret;
}

static int hm1246_setup_bus(struct hm1246 *hm1246)
{
	u64 polarity_ctrl = 0, pclk_ctrl = 0;
	int ret = 0;

	if (hm1246->mbus_flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		polarity_ctrl |= HM1246_POLARITY_CTRL_HSYNC;

	if (hm1246->mbus_flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		polarity_ctrl |= HM1246_POLARITY_CTRL_VSYNC;

	cci_write(hm1246->regmap, HM1246_POLARITY_CTRL_REG, polarity_ctrl,
		  &ret);

	/* If the clock output polarity flag PCLK_CTRL[3] is set (high), the
	 * data lines change state on the falling edge of PCLK and should
	 * therefore be sampled on the rising edge.
	 * This is different than described in the data sheet.
	 */
	if (hm1246->mbus_flags & V4L2_MBUS_PCLK_SAMPLE_RISING)
		pclk_ctrl |= HM1246_PCLK_CTRL_POL;

	cci_write(hm1246->regmap, HM1246_PCLK_CTRL_REG, pclk_ctrl, &ret);

	return ret;
}

static int hm1246_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct hm1246 *hm1246 = to_hm1246(sd);
	const struct hm1246_mode *mode = hm1246->cur_mode;
	int ret;

	ret = pm_runtime_resume_and_get(hm1246->dev);
	if (ret)
		return ret;

	ret = hm1246_identify_module(hm1246);
	if (ret)
		goto err_rpm_put;

	ret = hm1246_setup_pll(hm1246, mode);
	if (ret) {
		dev_err(hm1246->dev, "failed to setup PLL\n");
		goto err_rpm_put;
	}

	ret = hm1246_setup_moderegs(hm1246, mode);
	if (ret)
		goto err_rpm_put;

	ret = hm1246_setup_bus(hm1246);
	if (ret)
		goto err_rpm_put;

	ret = __v4l2_ctrl_handler_setup(&hm1246->ctrls);
	if (ret) {
		dev_err(hm1246->dev, "failed to setup v4l2 controls\n");
		goto err_rpm_put;
	}

	ret = cci_write(hm1246->regmap, HM1246_MODE_SELECT_REG,
			HM1246_MODE_SELECT_STREAM, NULL);
	if (ret)
		goto err_rpm_put;

	/* Since mirroring may change the actual pixel format, it must not be
	 * changed during streaming.
	 */
	__v4l2_ctrl_grab(hm1246->vflip_ctrl, true);
	__v4l2_ctrl_grab(hm1246->hflip_ctrl, true);

	return 0;

err_rpm_put:
	pm_runtime_put(hm1246->dev);

	return ret;
}

static int hm1246_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct hm1246 *hm1246 = to_hm1246(sd);
	int ret;

	ret = cci_write(hm1246->regmap, HM1246_MODE_SELECT_REG,
			HM1246_MODE_SELECT_STANDBY, NULL);

	__v4l2_ctrl_grab(hm1246->vflip_ctrl, false);
	__v4l2_ctrl_grab(hm1246->hflip_ctrl, false);

	pm_runtime_put(hm1246->dev);

	return ret;
}

static const struct v4l2_subdev_video_ops hm1246_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,

};

static const struct v4l2_subdev_pad_ops hm1246_subdev_pad_ops = {
	.enum_mbus_code = hm1246_enum_mbus_code,
	.enum_frame_size = hm1246_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = hm1246_set_format,
	.get_selection = hm1246_get_selection,
	.enable_streams = hm1246_enable_streams,
	.disable_streams = hm1246_disable_streams,
};

static int __maybe_unused hm1246_g_register(struct v4l2_subdev *sd,
					    struct v4l2_dbg_register *reg)
{
	struct hm1246 *hm1246 = to_hm1246(sd);
	u64 val;
	int ret;

	if (!pm_runtime_get_if_in_use(sd->dev))
		return 0;

	ret = cci_read(hm1246->regmap, CCI_REG8(reg->reg), &val, NULL);
	reg->val = val;

	pm_runtime_put(sd->dev);

	return ret;
}

static int __maybe_unused hm1246_s_register(struct v4l2_subdev *sd,
					    const struct v4l2_dbg_register *reg)
{
	struct hm1246 *hm1246 = to_hm1246(sd);
	int ret;

	if (!pm_runtime_get_if_in_use(sd->dev))
		return 0;

	ret = cci_write(hm1246->regmap, CCI_REG8(reg->reg), (u64)reg->val,
			NULL);

	pm_runtime_put(sd->dev);

	return ret;
}

static const struct v4l2_subdev_core_ops hm1246_core_ops = {
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register = hm1246_g_register,
	.s_register = hm1246_s_register,
#endif
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops hm1246_subdev_ops = {
	.core = &hm1246_core_ops,
	.video = &hm1246_video_ops,
	.pad = &hm1246_subdev_pad_ops,
};

static const struct v4l2_subdev_internal_ops hm1246_internal_ops = {
	.init_state = hm1246_init_state,
};

static int hm1246_get_regulators(struct device *dev, struct hm1246 *hm1246)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hm1246_supply_names); i++)
		hm1246->supplies[i].supply = hm1246_supply_names[i];

	return devm_regulator_bulk_get(dev, ARRAY_SIZE(hm1246_supply_names),
				       hm1246->supplies);
}

static int hm1246_parse_fwnode(struct hm1246 *hm1246)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_PARALLEL,
	};
	struct device *dev = hm1246->dev;
	int ret;

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
						   FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!endpoint)
		return dev_err_probe(dev, -EINVAL, "missing endpoint node\n");

	ret = v4l2_fwnode_endpoint_parse(endpoint, &bus_cfg);
	if (ret) {
		dev_err_probe(dev, ret, "parsing endpoint node failed\n");
		goto done;
	}

	hm1246->mbus_flags = bus_cfg.bus.parallel.flags;

done:
	fwnode_handle_put(endpoint);

	return ret;
}

static int hm1246_init_controls(struct hm1246 *hm1246)
{
	struct i2c_client *client = v4l2_get_subdevdata(&hm1246->sd);
	const struct hm1246_mode *mode = &hm1246_modes[0];
	const struct v4l2_ctrl_ops *ops = &hm1246_ctrl_ops;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	s64 pixel_rate, exposure_max, vblank_min, hblank;
	int ret;

	ctrl_hdlr = &hm1246->ctrls;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 9);
	if (ret)
		return ret;

	hm1246->hflip_ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &hm1246_ctrl_ops,
					       V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (hm1246->hflip_ctrl)
		hm1246->hflip_ctrl->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	hm1246->vflip_ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &hm1246_ctrl_ops,
					       V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (hm1246->vflip_ctrl)
		hm1246->vflip_ctrl->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	v4l2_ctrl_cluster(2, &hm1246->hflip_ctrl);

	hm1246->link_freq_ctrl =
		v4l2_ctrl_new_int_menu(ctrl_hdlr,
				       &hm1246_ctrl_ops,
				       V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(hm1246_link_freqs) - 1,
				       0,
				       hm1246_link_freqs);
	if (hm1246->link_freq_ctrl)
		hm1246->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = div_u64(hm1246_link_freqs[mode->link_freq_index],
			     mode->clocks_per_pixel);
	hm1246->pixel_rate_ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &hm1246_ctrl_ops,
						    V4L2_CID_PIXEL_RATE, 0,
						    pixel_rate, 1, pixel_rate);

	vblank_min = mode->vts_min - mode->height;
	hm1246->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &hm1246_ctrl_ops,
						V4L2_CID_VBLANK, vblank_min,
						HM1246_VTS_MAX - mode->height,
						1, vblank_min);

	hblank = mode->hts - mode->width;
	hm1246->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &hm1246_ctrl_ops,
						V4L2_CID_HBLANK, hblank, hblank,
						1, hblank);
	if (hm1246->hblank_ctrl)
		hm1246->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(ctrl_hdlr, &hm1246_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  HM1246_ANALOG_GLOBAL_GAIN_MIN,
			  HM1246_ANALOG_GLOBAL_GAIN_MAX,
			  HM1246_ANALOG_GLOBAL_GAIN_STEP,
			  HM1246_ANALOG_GLOBAL_GAIN_MIN);

	exposure_max = mode->vts_min - HM1246_COARSE_INTG_MARGIN;
	hm1246->exposure_ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &hm1246_ctrl_ops,
						  V4L2_CID_EXPOSURE,
						  HM1246_COARSE_INTG_MIN,
						  exposure_max,
						  HM1246_COARSE_INTG_STEP,
						  exposure_max);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &hm1246_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(hm1246_test_pattern_menu) - 1,
				     0, 0, hm1246_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto err_v4l2_ctrl_handler_free;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, ops, &props);
	if (ret)
		goto err_v4l2_ctrl_handler_free;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto err_v4l2_ctrl_handler_free;
	}

	hm1246->sd.ctrl_handler = ctrl_hdlr;

	return 0;

err_v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int hm1246_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct hm1246 *hm1246;
	int ret;

	hm1246 = devm_kzalloc(dev, sizeof(*hm1246), GFP_KERNEL);
	if (!hm1246)
		return -ENOMEM;

	hm1246->dev = dev;

	ret = hm1246_parse_fwnode(hm1246);
	if (ret)
		return ret;

	hm1246->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(hm1246->regmap))
		return dev_err_probe(dev, PTR_ERR(hm1246->regmap),
				     "failed to init CCI\n");

	hm1246->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(hm1246->xclk))
		return dev_err_probe(dev, PTR_ERR(hm1246->xclk),
				     "failed to get xclk\n");

	hm1246->xclk_freq = clk_get_rate(hm1246->xclk);
	if (hm1246->xclk_freq < HM1246_XCLK_MIN ||
	    hm1246->xclk_freq > HM1246_XCLK_MAX)
		dev_err_probe(dev, -EINVAL,
			      "xclk frequency out of range: %luHz\n",
			      hm1246->xclk_freq);

	ret = hm1246_get_regulators(dev, hm1246);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	hm1246->reset_gpio =
		devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(hm1246->reset_gpio))
		return dev_err_probe(dev, ret, "failed to get reset GPIO\n");

	v4l2_i2c_subdev_init(&hm1246->sd, client, &hm1246_subdev_ops);
	hm1246->sd.internal_ops = &hm1246_internal_ops;
	hm1246->cur_mode = &hm1246_modes[0];

	ret = hm1246_init_controls(hm1246);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init controls\n");

	hm1246->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	hm1246->pad.flags = MEDIA_PAD_FL_SOURCE;
	hm1246->sd.dev = &client->dev;
	hm1246->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&hm1246->sd.entity, 1, &hm1246->pad);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init media pads\n");
		goto err_v4l2_ctrl_handler_free;
	}

	hm1246->sd.state_lock = hm1246->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&hm1246->sd);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init v4l2 subdev\n");
		goto err_media_entity_cleanup;
	}

	ret = v4l2_async_register_subdev_sensor(&hm1246->sd);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register v4l2 subdev\n");
		goto err_subdev_cleanup;
	}

	pm_runtime_enable(hm1246->dev);
	pm_runtime_set_autosuspend_delay(hm1246->dev, 1000);
	pm_runtime_use_autosuspend(hm1246->dev);
	pm_runtime_idle(hm1246->dev);

	return 0;

err_subdev_cleanup:
	v4l2_subdev_cleanup(&hm1246->sd);
	pm_runtime_disable(hm1246->dev);
	pm_runtime_set_suspended(hm1246->dev);

err_media_entity_cleanup:
	media_entity_cleanup(&hm1246->sd.entity);

err_v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(&hm1246->ctrls);

	return ret;
}

static void hm1246_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct hm1246 *hm1246 = to_hm1246(sd);

	v4l2_async_unregister_subdev(&hm1246->sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&hm1246->sd.entity);
	v4l2_ctrl_handler_free(&hm1246->ctrls);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		hm1246_power_off(hm1246->dev);
		pm_runtime_set_suspended(&client->dev);
	}
}

static const struct of_device_id hm1246_of_match[] = {
	{ .compatible = "himax,hm1246" },
	{},
};
MODULE_DEVICE_TABLE(of, hm1246_of_match);

static DEFINE_RUNTIME_DEV_PM_OPS(hm1246_pm_ops, hm1246_power_off,
				 hm1246_power_on, NULL);

static struct i2c_driver hm1246_i2c_driver = {
	.driver = {
		.of_match_table = hm1246_of_match,
		.pm = pm_ptr(&hm1246_pm_ops),
		.name = "hm1246",
	},
	.probe = hm1246_probe,
	.remove = hm1246_remove,
};
module_i2c_driver(hm1246_i2c_driver);

MODULE_DESCRIPTION("Himax HM1246 camera driver");
MODULE_AUTHOR("Matthias Fend <matthias.fend@emfend.at>");
MODULE_LICENSE("GPL");
