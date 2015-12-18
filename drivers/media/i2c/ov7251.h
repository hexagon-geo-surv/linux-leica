/*
 * Support for OmniVision OV7251 1080p HD camera sensor.
 *
 * Copyright (c) 2013 Intel Corporation. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __OV7251_H__
#define __OV7251_H__
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <linux/spinlock.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>
#include <linux/atomisp_platform.h>
#include <linux/v4l2-mediabus.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>

#define OV7251_NAME		"ov7251"

/* Defines for register writes and register array processing */
#define I2C_MSG_LENGTH		0x2
#define I2C_RETRY_COUNT		5

#define OV7251_FOCAL_LENGTH_NUM	278	/*2.78mm*/
#define OV7251_FOCAL_LENGTH_DEM	100
#define OV7251_F_NUMBER_DEFAULT_NUM	26
#define OV7251_F_NUMBER_DEM	10

#define MAX_FMTS		1

/*
 * focal length bits definition:
 * bits 31-16: numerator, bits 15-0: denominator
 */
#define OV7251_FOCAL_LENGTH_DEFAULT 0x1160064

/*
 * current f-number bits definition:
 * bits 31-16: numerator, bits 15-0: denominator
 */
#define OV7251_F_NUMBER_DEFAULT 0x1a000a

/*
 * f-number range bits definition:
 * bits 31-24: max f-number numerator
 * bits 23-16: max f-number denominator
 * bits 15-8: min f-number numerator
 * bits 7-0: min f-number denominator
 */
#define OV7251_F_NUMBER_RANGE 0x1a0a1a0a
#define OV7251_ID	0x7251

#define OV7251_FINE_INTG_TIME_MIN 0
#define OV7251_FINE_INTG_TIME_MAX_MARGIN 0
#define OV7251_COARSE_INTG_TIME_MIN 1
#define OV7251_COARSE_INTG_TIME_MAX_MARGIN 4

/*
 * OV7251 System control registers
 */
#define OV7251_SW_SLEEP				0x0100
#define OV7251_SW_RESET				0x0103
#define OV7251_SW_STREAM			0x0100

#define OV7251_SC_CMMN_CHIP_ID_H		0x300A
#define OV7251_SC_CMMN_CHIP_ID_L		0x300B
#define OV7251_SC_CMMN_SCCB_ID			0x300C
#define OV7251_SC_CMMN_SUB_ID			0x302A /* process, version*/

#define OV7251_SC_CMMN_PAD_OEN0			0x3000
#define OV7251_SC_CMMN_PAD_OEN1			0x3001
#define OV7251_SC_CMMN_PAD_OEN2			0x3002
#define OV7251_SC_CMMN_PAD_OUT0			0x3008
#define OV7251_SC_CMMN_PAD_OUT1			0x3009
#define OV7251_SC_CMMN_PAD_OUT2			0x300D
#define OV7251_SC_CMMN_PAD_SEL0			0x300E
#define OV7251_SC_CMMN_PAD_SEL1			0x300F
#define OV7251_SC_CMMN_PAD_SEL2			0x3010

#define OV7251_SC_CMMN_PAD_PK			0x3011
#define OV7251_SC_CMMN_A_PWC_PK_O_13		0x3013
#define OV7251_SC_CMMN_A_PWC_PK_O_14		0x3014

#define OV7251_SC_CMMN_CLKRST0			0x301A
#define OV7251_SC_CMMN_CLKRST1			0x301B
#define OV7251_SC_CMMN_CLKRST2			0x301C
#define OV7251_SC_CMMN_CLKRST3			0x301D
#define OV7251_SC_CMMN_CLKRST4			0x301E
#define OV7251_SC_CMMN_CLKRST5			0x3005
#define OV7251_SC_CMMN_PCLK_DIV_CTRL		0x3007
#define OV7251_SC_CMMN_CLOCK_SEL		0x3020
#define OV7251_SC_SOC_CLKRST5			0x3040

#define OV7251_SC_CMMN_PLL_CTRL0		0x3034
#define OV7251_SC_CMMN_PLL_CTRL1		0x3035
#define OV7251_SC_CMMN_PLL_CTRL2		0x3039
#define OV7251_SC_CMMN_PLL_CTRL3		0x3037
#define OV7251_SC_CMMN_PLL_MULTIPLIER		0x3036
#define OV7251_SC_CMMN_PLL_DEBUG_OPT		0x3038
#define OV7251_SC_CMMN_PLLS_CTRL0		0x303A
#define OV7251_SC_CMMN_PLLS_CTRL1		0x303B
#define OV7251_SC_CMMN_PLLS_CTRL2		0x303C
#define OV7251_SC_CMMN_PLLS_CTRL3		0x303D

#define OV7251_SC_CMMN_MIPI_PHY_16		0x3016
#define OV7251_SC_CMMN_MIPI_PHY_17		0x3017
#define OV7251_SC_CMMN_MIPI_SC_CTRL_18		0x3018
#define OV7251_SC_CMMN_MIPI_SC_CTRL_19		0x3019
#define OV7251_SC_CMMN_MIPI_SC_CTRL_21		0x3021
#define OV7251_SC_CMMN_MIPI_SC_CTRL_22		0x3022

#define OV7251_AEC_PK_EXPO_H			0x3500
#define OV7251_AEC_PK_EXPO_M			0x3501
#define OV7251_AEC_PK_EXPO_L			0x3502
#define OV7251_AEC_MANUAL_CTRL			0x3503
#define OV7251_AGC_ADJ_H			0x3508
#define OV7251_AGC_ADJ_L			0x3509
#define OV7251_VTS_DIFF_H			0x350c
#define OV7251_VTS_DIFF_L			0x350d
#define OV7251_GROUP_ACCESS			0x3208
#define OV7251_HTS_H				0x380c
#define OV7251_HTS_L				0x380d
#define OV7251_VTS_H				0x380e
#define OV7251_VTS_L				0x380f

#define OV7251_MWB_GAIN_R_H			0x5186
#define OV7251_MWB_GAIN_R_L			0x5187
#define OV7251_MWB_GAIN_G_H			0x5188
#define OV7251_MWB_GAIN_G_L			0x5189
#define OV7251_MWB_GAIN_B_H			0x518a
#define OV7251_MWB_GAIN_B_L			0x518b

#define OV7251_H_CROP_START_H			0x3800
#define OV7251_H_CROP_START_L			0x3801
#define OV7251_V_CROP_START_H			0x3802
#define OV7251_V_CROP_START_L			0x3803
#define OV7251_H_CROP_END_H			0x3804
#define OV7251_H_CROP_END_L			0x3805
#define OV7251_V_CROP_END_H			0x3806
#define OV7251_V_CROP_END_L			0x3807
#define OV7251_H_OUTSIZE_H			0x3808
#define OV7251_H_OUTSIZE_L			0x3809
#define OV7251_V_OUTSIZE_H			0x380a
#define OV7251_V_OUTSIZE_L			0x380b

#define OV7251_START_STREAMING			0x01
#define OV7251_STOP_STREAMING			0x00

struct regval_list {
	u16 reg_num;
	u8 value;
};

struct ov7251_resolution {
	u8 *desc;
	const struct ov7251_reg *regs;
	int res;
	int width;
	int height;
	int fps;
	int pix_clk_freq;
	u32 skip_frames;
	u16 pixels_per_line;
	u16 lines_per_frame;
	u8 bin_factor_x;
	u8 bin_factor_y;
	u8 bin_mode;
	bool used;
	int mipi_freq;
};

struct ov7251_format {
	u8 *desc;
	u32 pixelformat;
	struct ov7251_reg *regs;
};

struct ov7251_control {
	struct v4l2_queryctrl qc;
	int (*query)(struct v4l2_subdev *sd, s32 *value);
	int (*tweak)(struct v4l2_subdev *sd, s32 value);
};

/*
 * ov7251 device structure.
 */
struct ov7251_device {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_mbus_framefmt format;
	struct mutex input_lock;
	struct i2c_client *ov_i2c_client;

	struct camera_sensor_platform_data *platform_data;
	int vt_pix_clk_freq_mhz;
	int fmt_idx;
	int run_mode;
	u16 pixels_per_line;
	u16 lines_per_frame;
	u8 res;
	u8 type;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
};

enum ov7251_tok_type {
	OV7251_8BIT  = 0x0001,
	OV7251_16BIT = 0x0002,
	OV7251_32BIT = 0x0004,
	OV7251_TOK_TERM   = 0xf000,	/* terminating token for reg list */
	OV7251_TOK_DELAY  = 0xfe00,	/* delay token for reg list */
	OV7251_TOK_MASK = 0xfff0
};

/**
 * struct ov7251_reg - MI sensor  register format
 * @type: type of the register
 * @reg: 16-bit offset to register
 * @val: 8/16/32-bit register value
 *
 * Define a structure for sensor register initialization values
 */
struct ov7251_reg {
	enum ov7251_tok_type type;
	u16 reg;
	u32 val;	/* @set value for read/mod/write, @mask */
};

#define to_ov7251_sensor(x) container_of(x, struct ov7251_device, sd)

#define OV7251_MAX_WRITE_BUF_SIZE	30

struct ov7251_write_buffer {
	u16 addr;
	u8 data[OV7251_MAX_WRITE_BUF_SIZE];
};

struct ov7251_write_ctrl {
	int index;
	struct ov7251_write_buffer buffer;
};

static const struct i2c_device_id ov7251_id[] = {
	{OV7251_NAME, 0},
	{"INT35AA", 0},
	{"INT35AA:00", 0},
	{}
};

/*
 * Register settings for various resolution
 */
static struct ov7251_reg const ov7251_QVGA_30fps[] = {
	{OV7251_8BIT, 0x3718, 0x10},
	{OV7251_8BIT, 0x3702, 0x0c},
	{OV7251_8BIT, 0x373a, 0x1c},
	{OV7251_8BIT, 0x3715, 0x01},
	{OV7251_8BIT, 0x3703, 0x0c},
	{OV7251_8BIT, 0x3705, 0x06},
	{OV7251_8BIT, 0x3730, 0x0e},
	{OV7251_8BIT, 0x3704, 0x1c},
	{OV7251_8BIT, 0x3f06, 0x00},
	{OV7251_8BIT, 0x371c, 0x00},
	{OV7251_8BIT, 0x371d, 0x46},
	{OV7251_8BIT, 0x371e, 0x00},
	{OV7251_8BIT, 0x371f, 0x63},
	{OV7251_8BIT, 0x3708, 0x61},
	{OV7251_8BIT, 0x3709, 0x12},
	{OV7251_8BIT, 0x3800, 0x01},
	{OV7251_8BIT, 0x3801, 0x42}, /* H crop start: 322 */
	{OV7251_8BIT, 0x3802, 0x00},
	{OV7251_8BIT, 0x3803, 0x20}, /* V crop start: 32 */
	{OV7251_8BIT, 0x3804, 0x06},
	{OV7251_8BIT, 0x3805, 0x95}, /* H crop end:  1685 */
	{OV7251_8BIT, 0x3806, 0x04},
	{OV7251_8BIT, 0x3807, 0x27}, /* V crop end:  1063 */
	{OV7251_8BIT, 0x3808, 0x01},
	{OV7251_8BIT, 0x3809, 0x50}, /* H output size: 336 */
	{OV7251_8BIT, 0x380a, 0x01},
	{OV7251_8BIT, 0x380b, 0x00}, /* V output size: 256 */

	/* H blank timing */
	{OV7251_8BIT, 0x380c, 0x08},
	{OV7251_8BIT, 0x380d, 0x00}, /* H total size: 2048 */
	{OV7251_8BIT, 0x380e, 0x04},
	{OV7251_8BIT, 0x380f, 0xa0}, /* V total size: 1184 */
	{OV7251_8BIT, 0x3810, 0x00},
	{OV7251_8BIT, 0x3811, 0x04}, /* H window offset: 5 */
	{OV7251_8BIT, 0x3812, 0x00},
	{OV7251_8BIT, 0x3813, 0x01}, /* V window offset: 2 */
	{OV7251_8BIT, 0x3820, 0xc0},
	{OV7251_8BIT, 0x3821, 0x06}, /* flip isp*/
	{OV7251_8BIT, 0x3814, 0x71},
	{OV7251_8BIT, 0x3815, 0x71},
	{OV7251_8BIT, 0x3612, 0x49},
	{OV7251_8BIT, 0x3618, 0x00},
	{OV7251_8BIT, 0x3a08, 0x01},
	{OV7251_8BIT, 0x3a09, 0xc3},
	{OV7251_8BIT, 0x3a0a, 0x01},
	{OV7251_8BIT, 0x3a0b, 0x77},
	{OV7251_8BIT, 0x3a0d, 0x00},
	{OV7251_8BIT, 0x3a0e, 0x00},
	{OV7251_8BIT, 0x4520, 0x09},
	{OV7251_8BIT, 0x4837, 0x1b},
	{OV7251_8BIT, 0x3000, 0xff},
	{OV7251_8BIT, 0x3001, 0xff},
	{OV7251_8BIT, 0x3002, 0xf0},
	{OV7251_8BIT, 0x3600, 0x08},
	{OV7251_8BIT, 0x3621, 0xc0},
	{OV7251_8BIT, 0x3632, 0x53}, /* added for power opt */
	{OV7251_8BIT, 0x3633, 0x63},
	{OV7251_8BIT, 0x3634, 0x24},
	{OV7251_8BIT, 0x3f01, 0x0c},
	{OV7251_8BIT, 0x5001, 0xc1}, /* v_en, h_en, blc_en */
	{OV7251_8BIT, 0x3614, 0xf0},
	{OV7251_8BIT, 0x3630, 0x2d},
	{OV7251_8BIT, 0x370b, 0x62},
	{OV7251_8BIT, 0x3706, 0x61},
	{OV7251_8BIT, 0x4000, 0x02},
	{OV7251_8BIT, 0x4002, 0xc5},
	{OV7251_8BIT, 0x4005, 0x08},
	{OV7251_8BIT, 0x404f, 0x84},
	{OV7251_8BIT, 0x4051, 0x00},
	{OV7251_8BIT, 0x5000, 0xff},
	{OV7251_8BIT, 0x3a18, 0x00},
	{OV7251_8BIT, 0x3a19, 0x80},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x4521, 0x00},
	{OV7251_8BIT, 0x5183, 0xb0}, /* AWB red */
	{OV7251_8BIT, 0x5184, 0xb0}, /* AWB green */
	{OV7251_8BIT, 0x5185, 0xb0}, /* AWB blue */
	{OV7251_8BIT, 0x5180, 0x03}, /* AWB manual mode */
	{OV7251_8BIT, 0x370c, 0x0c},
	{OV7251_8BIT, 0x4800, 0x24}, /* clk lane gate enable */
	{OV7251_8BIT, 0x3035, 0x00},
	{OV7251_8BIT, 0x3036, 0x26},
	{OV7251_8BIT, 0x3037, 0xa1},
	{OV7251_8BIT, 0x303e, 0x19},
	{OV7251_8BIT, 0x3038, 0x06},
	{OV7251_8BIT, 0x3018, 0x04},

	/* Added for power optimization */
	{OV7251_8BIT, 0x3000, 0x00},
	{OV7251_8BIT, 0x3001, 0x00},
	{OV7251_8BIT, 0x3002, 0x00},
	{OV7251_8BIT, 0x3a0f, 0x40},
	{OV7251_8BIT, 0x3a10, 0x38},
	{OV7251_8BIT, 0x3a1b, 0x48},
	{OV7251_8BIT, 0x3a1e, 0x30},
	{OV7251_8BIT, 0x3a11, 0x90},
	{OV7251_8BIT, 0x3a1f, 0x10},
	{OV7251_8BIT, 0x3011, 0x22},
	{OV7251_8BIT, 0x3a00, 0x58},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x3500, 0x00},
	{OV7251_8BIT, 0x3501, 0x46},
	{OV7251_8BIT, 0x3502, 0x00},
	{OV7251_8BIT, 0x3508, 0x00},
	{OV7251_8BIT, 0x3509, 0x10},
	{OV7251_TOK_TERM, 0, 0},

};

static struct ov7251_reg const ov7251_480P_30fps[] = {
/*	{OV7251_8BIT, 0x103, 0x1}, */
	{OV7251_8BIT, 0x100, 0x0},
	{OV7251_8BIT, 0x3005, 0x8},

	{OV7251_8BIT, 0x3012, 0xc0},
	{OV7251_8BIT, 0x3013, 0xd2},
	{OV7251_8BIT, 0x3014, 0x04},
	{OV7251_8BIT, 0x3016, 0x10},
	{OV7251_8BIT, 0x3017, 0x0},
	{OV7251_8BIT, 0x3018, 0x0},
	{OV7251_8BIT, 0x301a, 0x0},
	{OV7251_8BIT, 0x301b, 0x0},
	{OV7251_8BIT, 0x301c, 0x20},
	{OV7251_8BIT, 0x3023, 0x5},
	{OV7251_8BIT, 0x3037, 0xf0},

	{OV7251_8BIT, 0x3098, 0x4},
    {OV7251_8BIT, 0x3099, 0x32},
	{OV7251_8BIT, 0x309a, 0x5},
	{OV7251_8BIT, 0x309b, 0x4},
	{OV7251_8BIT, 0x309d, 0x0},

	{OV7251_8BIT, 0x30b0, 0xa},
	{OV7251_8BIT, 0x30b1, 0x1},
	{OV7251_8BIT, 0x30b3, 0x64},
	{OV7251_8BIT, 0x30b4, 0x3},
	{OV7251_8BIT, 0x30b5, 0x5},

/*tal check */
	{OV7251_8BIT, 0x3106, 0xda},

	{OV7251_8BIT, 0x3500, 0x00},
	{OV7251_8BIT, 0x3501, 0x35},
	{OV7251_8BIT, 0x3502, 0x20},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x3509, 0x10},
	{OV7251_8BIT, 0x350a, 0x0},
	{OV7251_8BIT, 0x350b, 0x0},

	{OV7251_8BIT, 0x3600, 0x1c},
	{OV7251_8BIT, 0x3602, 0x62},
	{OV7251_8BIT, 0x3620, 0xb7},
	{OV7251_8BIT, 0x3622, 0x4},
	{OV7251_8BIT, 0x3626, 0x21},
	{OV7251_8BIT, 0x3627, 0x30},
	{OV7251_8BIT, 0x3630, 0x44},
	{OV7251_8BIT, 0x3631, 0x35},
	{OV7251_8BIT, 0x3634, 0x60},
	{OV7251_8BIT, 0x3636, 0x0},
	{OV7251_8BIT, 0x3662, 0x1},
	{OV7251_8BIT, 0x3663, 0x70},
	{OV7251_8BIT, 0x3664, 0xf0},
	{OV7251_8BIT, 0x3666, 0xa},
	{OV7251_8BIT, 0x3669, 0x1a},
	{OV7251_8BIT, 0x366a, 0x0},
	{OV7251_8BIT, 0x366b, 0x50},
	{OV7251_8BIT, 0x3673, 0x1},
	{OV7251_8BIT, 0x3674, 0xff},
	{OV7251_8BIT, 0x3675, 0x3},
	{OV7251_8BIT, 0x3705, 0xc1},
	{OV7251_8BIT, 0x3709, 0x40},
	{OV7251_8BIT, 0x373c, 0x8},
	{OV7251_8BIT, 0x3742, 0x0},
	{OV7251_8BIT, 0x3757, 0xb3},
	{OV7251_8BIT, 0x3788, 0x0},


	{OV7251_8BIT, 0x37a8, 0x01},
	{OV7251_8BIT, 0x37a9, 0xc0},

	{OV7251_8BIT, 0x3800, 0x00},
	{OV7251_8BIT, 0x3801, 0x4}, /* H crop start: */
	{OV7251_8BIT, 0x3802, 0x00},
	{OV7251_8BIT, 0x3803, 0x4}, /* V crop start:  */
	{OV7251_8BIT, 0x3804, 0x02},
	{OV7251_8BIT, 0x3805, 0x8b}, /* H crop end: */
	{OV7251_8BIT, 0x3806, 0x01},
	{OV7251_8BIT, 0x3807, 0xeb}, /* V crop end:  */
	{OV7251_8BIT, 0x3808, 0x02},
	{OV7251_8BIT, 0x3809, 0x80}, /* H output size: */
	{OV7251_8BIT, 0x380a, 0x01},
	{OV7251_8BIT, 0x380b, 0xE0}, /* V output size: */

	/* H blank timing */
	{OV7251_8BIT, 0x380c, 0x03},
	{OV7251_8BIT, 0x380d, 0xA0}, /* H total size: */
	{OV7251_8BIT, 0x380e, 0x06},
	{OV7251_8BIT, 0x380f, 0xbc}, /* V total size: */
	{OV7251_8BIT, 0x3810, 0x00},
	{OV7251_8BIT, 0x3811, 0x04}, /* H window offset: 5 */
	{OV7251_8BIT, 0x3812, 0x00},
	{OV7251_8BIT, 0x3813, 0x01}, /* V window offset:  */

	{OV7251_8BIT, 0x3814, 0x11},
	{OV7251_8BIT, 0x3815, 0x11},

/*      Ziv Start */
	{OV7251_8BIT, 0x3820, 0x40},
	{OV7251_8BIT, 0x3821, 0x00},
	{OV7251_8BIT, 0x382f, 0x0e},
	{OV7251_8BIT, 0x3832, 0x00},
	{OV7251_8BIT, 0x3833, 0x05},
	{OV7251_8BIT, 0x3834, 0x00},
	{OV7251_8BIT, 0x3835, 0x0c},
	{OV7251_8BIT, 0x3837, 0x00},
	{OV7251_8BIT, 0x3b80, 0x00},

	{OV7251_8BIT, 0x3b81, 0xff}, /*strobe frame pattern */

	{OV7251_8BIT, 0x3b82, 0x10},
	{OV7251_8BIT, 0x3b83, 0x00},
	{OV7251_8BIT, 0x3b84, 0x08},
	{OV7251_8BIT, 0x3b85, 0x00},
	{OV7251_8BIT, 0x3b86, 0x01},
	{OV7251_8BIT, 0x3b87, 0x00},
	{OV7251_8BIT, 0x3b88, 0x00},
	{OV7251_8BIT, 0x3b89, 0x00},
	{OV7251_8BIT, 0x3b8a, 0x00},
	{OV7251_8BIT, 0x3b8b, 0x05},
	{OV7251_8BIT, 0x3b8c, 0x00},
	{OV7251_8BIT, 0x3b8d, 0x00},
	{OV7251_8BIT, 0x3b8e, 0x00},
	{OV7251_8BIT, 0x3b8f, 0x1a},

	{OV7251_8BIT, 0x3b94, 0x05},
	{OV7251_8BIT, 0x3b95, 0xf2},
	{OV7251_8BIT, 0x3b96, 0x40},

	{OV7251_8BIT, 0x3c00, 0x89},
	{OV7251_8BIT, 0x3c01, 0x63},
	{OV7251_8BIT, 0x3c02, 0x01},
	{OV7251_8BIT, 0x3c03, 0x00},
	{OV7251_8BIT, 0x3c04, 0x00},
	{OV7251_8BIT, 0x3c05, 0x03},
	{OV7251_8BIT, 0x3c06, 0x00},
	{OV7251_8BIT, 0x3c07, 0x06},

	{OV7251_8BIT, 0x3c0c, 0x01},
	{OV7251_8BIT, 0x3c0d, 0xd0},
	{OV7251_8BIT, 0x3c0e, 0x02},
	{OV7251_8BIT, 0x3c0f, 0x0a},

	{OV7251_8BIT, 0x4001, 0x42},

	{OV7251_8BIT, 0x4004, 0x04}, /* Number of bland line used */

	{OV7251_8BIT, 0x4005, 0x00},
	{OV7251_8BIT, 0x404e, 0x01},
	{OV7251_8BIT, 0x4300, 0xff},
	{OV7251_8BIT, 0x4301, 0x00},
	{OV7251_8BIT, 0x4501, 0x48},
	{OV7251_8BIT, 0x4600, 0x00},
	{OV7251_8BIT, 0x4601, 0x4e},
	{OV7251_8BIT, 0x4801, 0x0f},
	{OV7251_8BIT, 0x4806, 0x0f},
	{OV7251_8BIT, 0x4819, 0xaa},
	{OV7251_8BIT, 0x4823, 0x3e},

	{OV7251_8BIT, 0x4837, 0x1f}, /* PCLK PERIOD */

	{OV7251_8BIT, 0x4a0d, 0x00},
	{OV7251_8BIT, 0x4a47, 0x7f},
	{OV7251_8BIT, 0x4a49, 0xf0},
	{OV7251_8BIT, 0x4a4b, 0x30},

	{OV7251_8BIT, 0x5000, 0x85}, /* ISP CRTL00 */
	{OV7251_8BIT, 0x5001, 0x80}, /* ISP CRTL01 */

/*	Ziv End

//	{OV7251_8BIT, 0x5E00, 0x8C}, // test pattern, remove */

	{OV7251_TOK_TERM, 0, 0},
};

static struct ov7251_reg const ov7251_VGA_30fps[] = {
	{OV7251_8BIT, 0x3718, 0x10},
	{OV7251_8BIT, 0x3702, 0x18},
	{OV7251_8BIT, 0x373a, 0x3c},
	{OV7251_8BIT, 0x3715, 0x01},
	{OV7251_8BIT, 0x3703, 0x1d},
	{OV7251_8BIT, 0x3705, 0x12},
	{OV7251_8BIT, 0x3730, 0x1f},
	{OV7251_8BIT, 0x3704, 0x3f},
	{OV7251_8BIT, 0x3f06, 0x1d},
	{OV7251_8BIT, 0x371c, 0x00},
	{OV7251_8BIT, 0x371d, 0x83},
	{OV7251_8BIT, 0x371e, 0x00},
	{OV7251_8BIT, 0x371f, 0xbd},
	{OV7251_8BIT, 0x3708, 0x63},
	{OV7251_8BIT, 0x3709, 0x52},
	{OV7251_8BIT, 0x3800, 0x01},
	{OV7251_8BIT, 0x3801, 0x42}, /* H crop start: 322 */
	{OV7251_8BIT, 0x3802, 0x00},
	{OV7251_8BIT, 0x3803, 0x20}, /* V crop start:  32*/
	{OV7251_8BIT, 0x3804, 0x06},
	{OV7251_8BIT, 0x3805, 0x6B}, /* H crop end:   1643*/
	{OV7251_8BIT, 0x3806, 0x04},
	{OV7251_8BIT, 0x3807, 0x03}, /* V crop end:   1027*/
	{OV7251_8BIT, 0x3808, 0x02},
	{OV7251_8BIT, 0x3809, 0x90}, /* H output size: 656 */
	{OV7251_8BIT, 0x380a, 0x01},
	{OV7251_8BIT, 0x380b, 0xF0}, /* V output size: 496 */

	/* H blank timing */
	{OV7251_8BIT, 0x380c, 0x08},
	{OV7251_8BIT, 0x380d, 0x00}, /* H total size: 2048 */
	{OV7251_8BIT, 0x380e, 0x04},
	{OV7251_8BIT, 0x380f, 0xa0}, /* V total size: 1184 */
	{OV7251_8BIT, 0x3810, 0x00},
	{OV7251_8BIT, 0x3811, 0x04}, /* H window offset: 5 */
	{OV7251_8BIT, 0x3812, 0x00},
	{OV7251_8BIT, 0x3813, 0x01}, /* V window offset: 2 */
	{OV7251_8BIT, 0x3820, 0x80},
	{OV7251_8BIT, 0x3821, 0x06}, /* flip isp*/
	{OV7251_8BIT, 0x3814, 0x31},
	{OV7251_8BIT, 0x3815, 0x31},
	{OV7251_8BIT, 0x3612, 0x4b},
	{OV7251_8BIT, 0x3618, 0x04},
	{OV7251_8BIT, 0x3a08, 0x02},
	{OV7251_8BIT, 0x3a09, 0x67},
	{OV7251_8BIT, 0x3a0a, 0x02},
	{OV7251_8BIT, 0x3a0b, 0x00},
	{OV7251_8BIT, 0x3a0d, 0x00},
	{OV7251_8BIT, 0x3a0e, 0x00},
	{OV7251_8BIT, 0x4520, 0x0a},
	{OV7251_8BIT, 0x4837, 0x29},
	{OV7251_8BIT, 0x3000, 0xff},
	{OV7251_8BIT, 0x3001, 0xff},
	{OV7251_8BIT, 0x3002, 0xf0},
	{OV7251_8BIT, 0x3600, 0x08},
	{OV7251_8BIT, 0x3621, 0xc0},
	{OV7251_8BIT, 0x3632, 0x53}, /* added for power opt */
	{OV7251_8BIT, 0x3633, 0x63},
	{OV7251_8BIT, 0x3634, 0x24},
	{OV7251_8BIT, 0x3f01, 0x0c},
	{OV7251_8BIT, 0x5001, 0xc1}, /* v_en, h_en, blc_en */
	{OV7251_8BIT, 0x3614, 0xf0},
	{OV7251_8BIT, 0x3630, 0x2d},
	{OV7251_8BIT, 0x370b, 0x62},
	{OV7251_8BIT, 0x3706, 0x61},
	{OV7251_8BIT, 0x4000, 0x02},
	{OV7251_8BIT, 0x4002, 0xc5},
	{OV7251_8BIT, 0x4005, 0x08},
	{OV7251_8BIT, 0x404f, 0x84},
	{OV7251_8BIT, 0x4051, 0x00},
	{OV7251_8BIT, 0x5000, 0xff},
	{OV7251_8BIT, 0x3a18, 0x00},
	{OV7251_8BIT, 0x3a19, 0x80},
	{OV7251_8BIT, 0x3503, 0x00},
	{OV7251_8BIT, 0x4521, 0x00},
	{OV7251_8BIT, 0x5183, 0xb0}, /* AWB red */
	{OV7251_8BIT, 0x5184, 0xb0}, /* AWB green */
	{OV7251_8BIT, 0x5185, 0xb0}, /* AWB blue */
	{OV7251_8BIT, 0x5180, 0x03}, /* AWB manual mode */
	{OV7251_8BIT, 0x370c, 0x0c},
	{OV7251_8BIT, 0x4800, 0x24}, /* clk lane gate enable */
	{OV7251_8BIT, 0x3035, 0x00},
	{OV7251_8BIT, 0x3036, 0x26},
	{OV7251_8BIT, 0x3037, 0xa1},
	{OV7251_8BIT, 0x303e, 0x19},
	{OV7251_8BIT, 0x3038, 0x06},
	{OV7251_8BIT, 0x3018, 0x04},

	/* Added for power optimization */
	{OV7251_8BIT, 0x3000, 0x00},
	{OV7251_8BIT, 0x3001, 0x00},
	{OV7251_8BIT, 0x3002, 0x00},
	{OV7251_8BIT, 0x3a0f, 0x40},
	{OV7251_8BIT, 0x3a10, 0x38},
	{OV7251_8BIT, 0x3a1b, 0x48},
	{OV7251_8BIT, 0x3a1e, 0x30},
	{OV7251_8BIT, 0x3a11, 0x90},
	{OV7251_8BIT, 0x3a1f, 0x10},
	{OV7251_8BIT, 0x3011, 0x22},
	{OV7251_8BIT, 0x3a00, 0x58},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x3500, 0x00},
	{OV7251_8BIT, 0x3501, 0x46},
	{OV7251_8BIT, 0x3502, 0x00},
	{OV7251_8BIT, 0x3508, 0x00},
	{OV7251_8BIT, 0x3509, 0x10},
	{OV7251_TOK_TERM, 0, 0},
};

static struct ov7251_reg const ov7251_1632_1092_30fps[] = {
	{OV7251_8BIT, 0x3021, 0x03}, /* For stand wait for
				a whole frame complete.(vblank) */
	{OV7251_8BIT, 0x3718, 0x10},
	{OV7251_8BIT, 0x3702, 0x24},
	{OV7251_8BIT, 0x373a, 0x60},
	{OV7251_8BIT, 0x3715, 0x01},
	{OV7251_8BIT, 0x3703, 0x2e},
	{OV7251_8BIT, 0x3705, 0x10},
	{OV7251_8BIT, 0x3730, 0x30},
	{OV7251_8BIT, 0x3704, 0x62},
	{OV7251_8BIT, 0x3f06, 0x3a},
	{OV7251_8BIT, 0x371c, 0x00},
	{OV7251_8BIT, 0x371d, 0xc4},
	{OV7251_8BIT, 0x371e, 0x01},
	{OV7251_8BIT, 0x371f, 0x0d},
	{OV7251_8BIT, 0x3708, 0x61},
	{OV7251_8BIT, 0x3709, 0x12},
	{OV7251_8BIT, 0x3800, 0x00},
	{OV7251_8BIT, 0x3801, 0x9E}, /* H crop start: 158 */
	{OV7251_8BIT, 0x3802, 0x00},
	{OV7251_8BIT, 0x3803, 0x01}, /* V crop start: 1 */
	{OV7251_8BIT, 0x3804, 0x07},
	{OV7251_8BIT, 0x3805, 0x05}, /* H crop end: 1797 */
	{OV7251_8BIT, 0x3806, 0x04},
	{OV7251_8BIT, 0x3807, 0x45}, /* V crop end: 1093 */

	{OV7251_8BIT, 0x3808, 0x06},
	{OV7251_8BIT, 0x3809, 0x60}, /* H output size: 1632 */
	{OV7251_8BIT, 0x380a, 0x04},
	{OV7251_8BIT, 0x380b, 0x44}, /* V output size: 1092 */
	{OV7251_8BIT, 0x380c, 0x08},
	{OV7251_8BIT, 0x380d, 0xd4}, /* H timing: 2260 */
	{OV7251_8BIT, 0x380e, 0x04},
	{OV7251_8BIT, 0x380f, 0xdc}, /* V timing: 1244 */
	{OV7251_8BIT, 0x3810, 0x00},
	{OV7251_8BIT, 0x3811, 0x03}, /* H window offset: 3 */
	{OV7251_8BIT, 0x3812, 0x00},
	{OV7251_8BIT, 0x3813, 0x02}, /* V window offset: 2 */
	{OV7251_8BIT, 0x3820, 0x80},
	{OV7251_8BIT, 0x3821, 0x06}, /*  mirror */
	{OV7251_8BIT, 0x3814, 0x11},
	{OV7251_8BIT, 0x3815, 0x11},
	{OV7251_8BIT, 0x3612, 0x0b},
	{OV7251_8BIT, 0x3618, 0x04},
	{OV7251_8BIT, 0x3a08, 0x01},
	{OV7251_8BIT, 0x3a09, 0x50},
	{OV7251_8BIT, 0x3a0a, 0x01},
	{OV7251_8BIT, 0x3a0b, 0x18},
	{OV7251_8BIT, 0x3a0d, 0x03},
	{OV7251_8BIT, 0x3a0e, 0x03},
	{OV7251_8BIT, 0x4520, 0x00},
	{OV7251_8BIT, 0x4837, 0x1b},
	{OV7251_8BIT, 0x3600, 0x08},
	{OV7251_8BIT, 0x3621, 0xc0},
	{OV7251_8BIT, 0x3632, 0xd2}, /* added for power opt */
	{OV7251_8BIT, 0x3633, 0x23},
	{OV7251_8BIT, 0x3634, 0x54},
	{OV7251_8BIT, 0x3f01, 0x0c},
	{OV7251_8BIT, 0x5001, 0xc1},
	{OV7251_8BIT, 0x3614, 0xf0},
	{OV7251_8BIT, 0x3630, 0x2d},
	{OV7251_8BIT, 0x370b, 0x62},
	{OV7251_8BIT, 0x3706, 0x61},
	{OV7251_8BIT, 0x4000, 0x02},
	{OV7251_8BIT, 0x4002, 0xc5},
	{OV7251_8BIT, 0x4005, 0x08},
	{OV7251_8BIT, 0x404f, 0x84},
	{OV7251_8BIT, 0x4051, 0x00},
	{OV7251_8BIT, 0x5000, 0xcf}, /* manual 3a */
	{OV7251_8BIT, 0x301d, 0xf0}, /* enable group hold */
	{OV7251_8BIT, 0x3a18, 0x00},
	{OV7251_8BIT, 0x3a19, 0x80},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x4521, 0x00},
	{OV7251_8BIT, 0x5183, 0xb0},
	{OV7251_8BIT, 0x5184, 0xb0},
	{OV7251_8BIT, 0x5185, 0xb0},
	{OV7251_8BIT, 0x370c, 0x0c},
	{OV7251_8BIT, 0x3035, 0x00},
	{OV7251_8BIT, 0x3036, 0x2c}, /* 422.4 MHz */
	{OV7251_8BIT, 0x3037, 0xa1},
	{OV7251_8BIT, 0x303e, 0x19},
	{OV7251_8BIT, 0x3038, 0x06},
	{OV7251_8BIT, 0x3018, 0x04},
	{OV7251_8BIT, 0x3000, 0x00}, /* added for power optimization */
	{OV7251_8BIT, 0x3001, 0x00},
	{OV7251_8BIT, 0x3002, 0x00},
	{OV7251_8BIT, 0x3a0f, 0x40},
	{OV7251_8BIT, 0x3a10, 0x38},
	{OV7251_8BIT, 0x3a1b, 0x48},
	{OV7251_8BIT, 0x3a1e, 0x30},
	{OV7251_8BIT, 0x3a11, 0x90},
	{OV7251_8BIT, 0x3a1f, 0x10},
	{OV7251_8BIT, 0x3503, 0x07}, /* manual 3a */
	{OV7251_8BIT, 0x3500, 0x00},
	{OV7251_8BIT, 0x3501, 0x3F},
	{OV7251_8BIT, 0x3502, 0x00},
	{OV7251_8BIT, 0x3508, 0x00},
	{OV7251_8BIT, 0x3509, 0x00},
	{OV7251_TOK_TERM, 0, 0}
};

static struct ov7251_reg const ov7251_1452_1092_30fps[] = {
	{OV7251_8BIT, 0x3021, 0x03}, /* For stand wait for
				a whole frame complete.(vblank) */
	{OV7251_8BIT, 0x3718, 0x10},
	{OV7251_8BIT, 0x3702, 0x24},
	{OV7251_8BIT, 0x373a, 0x60},
	{OV7251_8BIT, 0x3715, 0x01},
	{OV7251_8BIT, 0x3703, 0x2e},
	{OV7251_8BIT, 0x3705, 0x10},
	{OV7251_8BIT, 0x3730, 0x30},
	{OV7251_8BIT, 0x3704, 0x62},
	{OV7251_8BIT, 0x3f06, 0x3a},
	{OV7251_8BIT, 0x371c, 0x00},
	{OV7251_8BIT, 0x371d, 0xc4},
	{OV7251_8BIT, 0x371e, 0x01},
	{OV7251_8BIT, 0x371f, 0x0d},
	{OV7251_8BIT, 0x3708, 0x61},
	{OV7251_8BIT, 0x3709, 0x12},
	{OV7251_8BIT, 0x3800, 0x00},
	{OV7251_8BIT, 0x3801, 0xF8}, /* H crop start: 248 */
	{OV7251_8BIT, 0x3802, 0x00},
	{OV7251_8BIT, 0x3803, 0x01}, /* V crop start: 1 */
	{OV7251_8BIT, 0x3804, 0x06},
	{OV7251_8BIT, 0x3805, 0xab}, /* H crop end: 1707 */
	{OV7251_8BIT, 0x3806, 0x04},
	{OV7251_8BIT, 0x3807, 0x45}, /* V crop end: 1093 */
	{OV7251_8BIT, 0x3808, 0x05},
	{OV7251_8BIT, 0x3809, 0xac}, /* H output size: 1452 */
	{OV7251_8BIT, 0x380a, 0x04},
	{OV7251_8BIT, 0x380b, 0x44}, /* V output size: 1092 */
	{OV7251_8BIT, 0x380c, 0x08},
	{OV7251_8BIT, 0x380d, 0xd4}, /* H timing: 2260 */
	{OV7251_8BIT, 0x380e, 0x04},
	{OV7251_8BIT, 0x380f, 0xdc}, /* V timing: 1244 */
	{OV7251_8BIT, 0x3810, 0x00},
	{OV7251_8BIT, 0x3811, 0x03}, /* H window offset: 3 */
	{OV7251_8BIT, 0x3812, 0x00},
	{OV7251_8BIT, 0x3813, 0x02}, /* V window offset: 2 */
	{OV7251_8BIT, 0x3820, 0x80},
	{OV7251_8BIT, 0x3821, 0x06}, /*  mirror */
	{OV7251_8BIT, 0x3814, 0x11},
	{OV7251_8BIT, 0x3815, 0x11},
	{OV7251_8BIT, 0x3612, 0x0b},
	{OV7251_8BIT, 0x3618, 0x04},
	{OV7251_8BIT, 0x3a08, 0x01},
	{OV7251_8BIT, 0x3a09, 0x50},
	{OV7251_8BIT, 0x3a0a, 0x01},
	{OV7251_8BIT, 0x3a0b, 0x18},
	{OV7251_8BIT, 0x3a0d, 0x03},
	{OV7251_8BIT, 0x3a0e, 0x03},
	{OV7251_8BIT, 0x4520, 0x00},
	{OV7251_8BIT, 0x4837, 0x1b},
	{OV7251_8BIT, 0x3600, 0x08},
	{OV7251_8BIT, 0x3621, 0xc0},
	{OV7251_8BIT, 0x3632, 0xd2}, /* added for power opt */
	{OV7251_8BIT, 0x3633, 0x23},
	{OV7251_8BIT, 0x3634, 0x54},
	{OV7251_8BIT, 0x3f01, 0x0c},
	{OV7251_8BIT, 0x5001, 0xc1},
	{OV7251_8BIT, 0x3614, 0xf0},
	{OV7251_8BIT, 0x3630, 0x2d},
	{OV7251_8BIT, 0x370b, 0x62},
	{OV7251_8BIT, 0x3706, 0x61},
	{OV7251_8BIT, 0x4000, 0x02},
	{OV7251_8BIT, 0x4002, 0xc5},
	{OV7251_8BIT, 0x4005, 0x08},
	{OV7251_8BIT, 0x404f, 0x84},
	{OV7251_8BIT, 0x4051, 0x00},
	{OV7251_8BIT, 0x5000, 0xcf}, /* manual 3a */
	{OV7251_8BIT, 0x301d, 0xf0}, /* enable group hold */
	{OV7251_8BIT, 0x3a18, 0x00},
	{OV7251_8BIT, 0x3a19, 0x80},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x4521, 0x00},
	{OV7251_8BIT, 0x5183, 0xb0},
	{OV7251_8BIT, 0x5184, 0xb0},
	{OV7251_8BIT, 0x5185, 0xb0},
	{OV7251_8BIT, 0x370c, 0x0c},
	{OV7251_8BIT, 0x3035, 0x00},
	{OV7251_8BIT, 0x3036, 0x2c}, /* 422.4 MHz */
	{OV7251_8BIT, 0x3037, 0xa1},
	{OV7251_8BIT, 0x303e, 0x19},
	{OV7251_8BIT, 0x3038, 0x06},
	{OV7251_8BIT, 0x3018, 0x04},
	{OV7251_8BIT, 0x3000, 0x00}, /* added for power optimization */
	{OV7251_8BIT, 0x3001, 0x00},
	{OV7251_8BIT, 0x3002, 0x00},
	{OV7251_8BIT, 0x3a0f, 0x40},
	{OV7251_8BIT, 0x3a10, 0x38},
	{OV7251_8BIT, 0x3a1b, 0x48},
	{OV7251_8BIT, 0x3a1e, 0x30},
	{OV7251_8BIT, 0x3a11, 0x90},
	{OV7251_8BIT, 0x3a1f, 0x10},
	{OV7251_8BIT, 0x3503, 0x07}, /* manual 3a */
	{OV7251_8BIT, 0x3500, 0x00},
	{OV7251_8BIT, 0x3501, 0x3F},
	{OV7251_8BIT, 0x3502, 0x00},
	{OV7251_8BIT, 0x3508, 0x00},
	{OV7251_8BIT, 0x3509, 0x00},
	{OV7251_TOK_TERM, 0, 0}
};
static struct ov7251_reg const ov7251_1M3_30fps[] = {
	{OV7251_8BIT, 0x3718, 0x10},
	{OV7251_8BIT, 0x3702, 0x24},
	{OV7251_8BIT, 0x373a, 0x60},
	{OV7251_8BIT, 0x3715, 0x01},
	{OV7251_8BIT, 0x3703, 0x2e},
	{OV7251_8BIT, 0x3705, 0x10},
	{OV7251_8BIT, 0x3730, 0x30},
	{OV7251_8BIT, 0x3704, 0x62},
	{OV7251_8BIT, 0x3f06, 0x3a},
	{OV7251_8BIT, 0x371c, 0x00},
	{OV7251_8BIT, 0x371d, 0xc4},
	{OV7251_8BIT, 0x371e, 0x01},
	{OV7251_8BIT, 0x371f, 0x0d},
	{OV7251_8BIT, 0x3708, 0x61},
	{OV7251_8BIT, 0x3709, 0x12},
	{OV7251_8BIT, 0x3800, 0x01},
	{OV7251_8BIT, 0x3801, 0x4a},	/* H crop start: 330 */
	{OV7251_8BIT, 0x3802, 0x00},
	{OV7251_8BIT, 0x3803, 0x03},	/* V crop start: 3 */
	{OV7251_8BIT, 0x3804, 0x06},
	{OV7251_8BIT, 0x3805, 0xe1},	/* H crop end:  1761 */
	{OV7251_8BIT, 0x3806, 0x04},
	{OV7251_8BIT, 0x3807, 0x47},	/* V crop end:  1095 */
	{OV7251_8BIT, 0x3808, 0x05},
	{OV7251_8BIT, 0x3809, 0x88},	/* H output size: 1416 */
	{OV7251_8BIT, 0x380a, 0x04},
	{OV7251_8BIT, 0x380b, 0x0a},	/* V output size: 1034 */

	/* H blank timing */
	{OV7251_8BIT, 0x380c, 0x08},
	{OV7251_8BIT, 0x380d, 0x00},	/* H total size: 2048 */
	{OV7251_8BIT, 0x380e, 0x04},
	{OV7251_8BIT, 0x380f, 0xa0},	/* V total size: 1184 */
	{OV7251_8BIT, 0x3810, 0x00},
	{OV7251_8BIT, 0x3811, 0x05},	/* H window offset: 5 */
	{OV7251_8BIT, 0x3812, 0x00},
	{OV7251_8BIT, 0x3813, 0x02},	/* V window offset: 2 */
	{OV7251_8BIT, 0x3820, 0x80},
	{OV7251_8BIT, 0x3821, 0x06},	/* flip isp */
	{OV7251_8BIT, 0x3814, 0x11},
	{OV7251_8BIT, 0x3815, 0x11},
	{OV7251_8BIT, 0x3612, 0x0b},
	{OV7251_8BIT, 0x3618, 0x04},
	{OV7251_8BIT, 0x3a08, 0x01},
	{OV7251_8BIT, 0x3a09, 0x50},
	{OV7251_8BIT, 0x3a0a, 0x01},
	{OV7251_8BIT, 0x3a0b, 0x18},
	{OV7251_8BIT, 0x3a0d, 0x03},
	{OV7251_8BIT, 0x3a0e, 0x03},
	{OV7251_8BIT, 0x4520, 0x00},
	{OV7251_8BIT, 0x4837, 0x1b},
	{OV7251_8BIT, 0x3000, 0xff},
	{OV7251_8BIT, 0x3001, 0xff},
	{OV7251_8BIT, 0x3002, 0xf0},
	{OV7251_8BIT, 0x3600, 0x08},
	{OV7251_8BIT, 0x3621, 0xc0},
	{OV7251_8BIT, 0x3632, 0xd2},	/* added for power opt */
	{OV7251_8BIT, 0x3633, 0x23},
	{OV7251_8BIT, 0x3634, 0x54},
	{OV7251_8BIT, 0x3f01, 0x0c},
	{OV7251_8BIT, 0x5001, 0xc1},	/* v_en, h_en, blc_en */
	{OV7251_8BIT, 0x3614, 0xf0},
	{OV7251_8BIT, 0x3630, 0x2d},
	{OV7251_8BIT, 0x370b, 0x62},
	{OV7251_8BIT, 0x3706, 0x61},
	{OV7251_8BIT, 0x4000, 0x02},
	{OV7251_8BIT, 0x4002, 0xc5},
	{OV7251_8BIT, 0x4005, 0x08},
	{OV7251_8BIT, 0x404f, 0x84},
	{OV7251_8BIT, 0x4051, 0x00},
	{OV7251_8BIT, 0x5000, 0xcf},
	{OV7251_8BIT, 0x3a18, 0x00},
	{OV7251_8BIT, 0x3a19, 0x80},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x4521, 0x00},
	{OV7251_8BIT, 0x5183, 0xb0},	/* AWB red */
	{OV7251_8BIT, 0x5184, 0xb0},	/* AWB green */
	{OV7251_8BIT, 0x5185, 0xb0},	/* AWB blue */
	{OV7251_8BIT, 0x5180, 0x03},	/* AWB manual mode */
	{OV7251_8BIT, 0x370c, 0x0c},
	{OV7251_8BIT, 0x4800, 0x24},	/* clk lane gate enable */
	{OV7251_8BIT, 0x3035, 0x00},
	{OV7251_8BIT, 0x3036, 0x26},
	{OV7251_8BIT, 0x3037, 0xa1},
	{OV7251_8BIT, 0x303e, 0x19},
	{OV7251_8BIT, 0x3038, 0x06},
	{OV7251_8BIT, 0x3018, 0x04},

	/* Added for power optimization */
	{OV7251_8BIT, 0x3000, 0x00},
	{OV7251_8BIT, 0x3001, 0x00},
	{OV7251_8BIT, 0x3002, 0x00},
	{OV7251_8BIT, 0x3a0f, 0x40},
	{OV7251_8BIT, 0x3a10, 0x38},
	{OV7251_8BIT, 0x3a1b, 0x48},
	{OV7251_8BIT, 0x3a1e, 0x30},
	{OV7251_8BIT, 0x3a11, 0x90},
	{OV7251_8BIT, 0x3a1f, 0x10},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x3500, 0x00},
	{OV7251_8BIT, 0x3501, 0x46},
	{OV7251_8BIT, 0x3502, 0x00},
	{OV7251_8BIT, 0x3508, 0x00},
	{OV7251_8BIT, 0x3509, 0x10},
	{OV7251_TOK_TERM, 0, 0},
};

static struct ov7251_reg const ov7251_1080p_30fps[] = {
	{OV7251_8BIT, 0x3021, 0x03}, /* For stand wait for
				a whole frame complete.(vblank) */
	{OV7251_8BIT, 0x3718, 0x10},
	{OV7251_8BIT, 0x3702, 0x24},
	{OV7251_8BIT, 0x373a, 0x60},
	{OV7251_8BIT, 0x3715, 0x01},
	{OV7251_8BIT, 0x3703, 0x2e},
	{OV7251_8BIT, 0x3705, 0x10},
	{OV7251_8BIT, 0x3730, 0x30},
	{OV7251_8BIT, 0x3704, 0x62},
	{OV7251_8BIT, 0x3f06, 0x3a},
	{OV7251_8BIT, 0x371c, 0x00},
	{OV7251_8BIT, 0x371d, 0xc4},
	{OV7251_8BIT, 0x371e, 0x01},
	{OV7251_8BIT, 0x371f, 0x0d},
	{OV7251_8BIT, 0x3708, 0x61},
	{OV7251_8BIT, 0x3709, 0x12},
	{OV7251_8BIT, 0x3800, 0x00},
	{OV7251_8BIT, 0x3801, 0x08}, /* H crop start: 8 */
	{OV7251_8BIT, 0x3802, 0x00},
	{OV7251_8BIT, 0x3803, 0x01}, /* V crop start: 1 */
	{OV7251_8BIT, 0x3804, 0x07},
	{OV7251_8BIT, 0x3805, 0x9b}, /* H crop end: 1947 */
	{OV7251_8BIT, 0x3806, 0x04},
	{OV7251_8BIT, 0x3807, 0x45}, /* V crop end: 1093 */
	{OV7251_8BIT, 0x3808, 0x07},
	{OV7251_8BIT, 0x3809, 0x8c}, /* H output size: 1932 */
	{OV7251_8BIT, 0x380a, 0x04},
	{OV7251_8BIT, 0x380b, 0x44}, /* V output size: 1092 */
	{OV7251_8BIT, 0x380c, 0x08},
	{OV7251_8BIT, 0x380d, 0xd4}, /* H timing: 2260 */
	{OV7251_8BIT, 0x380e, 0x04},
	{OV7251_8BIT, 0x380f, 0xdc}, /* V timing: 1244 */
	{OV7251_8BIT, 0x3810, 0x00},
	{OV7251_8BIT, 0x3811, 0x03}, /* H window offset: 3 */
	{OV7251_8BIT, 0x3812, 0x00},
	{OV7251_8BIT, 0x3813, 0x02}, /* V window offset: 2 */
	{OV7251_8BIT, 0x3820, 0x80},
	{OV7251_8BIT, 0x3821, 0x06}, /*  mirror */
	{OV7251_8BIT, 0x3814, 0x11},
	{OV7251_8BIT, 0x3815, 0x11},
	{OV7251_8BIT, 0x3612, 0x0b},
	{OV7251_8BIT, 0x3618, 0x04},
	{OV7251_8BIT, 0x3a08, 0x01},
	{OV7251_8BIT, 0x3a09, 0x50},
	{OV7251_8BIT, 0x3a0a, 0x01},
	{OV7251_8BIT, 0x3a0b, 0x18},
	{OV7251_8BIT, 0x3a0d, 0x03},
	{OV7251_8BIT, 0x3a0e, 0x03},
	{OV7251_8BIT, 0x4520, 0x00},
	{OV7251_8BIT, 0x4837, 0x1b},
	{OV7251_8BIT, 0x3600, 0x08},
	{OV7251_8BIT, 0x3621, 0xc0},
	{OV7251_8BIT, 0x3632, 0xd2}, /* added for power opt */
	{OV7251_8BIT, 0x3633, 0x23},
	{OV7251_8BIT, 0x3634, 0x54},
	{OV7251_8BIT, 0x3f01, 0x0c},
	{OV7251_8BIT, 0x5001, 0xc1},
	{OV7251_8BIT, 0x3614, 0xf0},
	{OV7251_8BIT, 0x3630, 0x2d},
	{OV7251_8BIT, 0x370b, 0x62},
	{OV7251_8BIT, 0x3706, 0x61},
	{OV7251_8BIT, 0x4000, 0x02},
	{OV7251_8BIT, 0x4002, 0xc5},
	{OV7251_8BIT, 0x4005, 0x08},
	{OV7251_8BIT, 0x404f, 0x84},
	{OV7251_8BIT, 0x4051, 0x00},
	{OV7251_8BIT, 0x5000, 0xcf}, /* manual 3a */
	{OV7251_8BIT, 0x301d, 0xf0}, /* enable group hold */
	{OV7251_8BIT, 0x3a18, 0x00},
	{OV7251_8BIT, 0x3a19, 0x80},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x4521, 0x00},
	{OV7251_8BIT, 0x5183, 0xb0},
	{OV7251_8BIT, 0x5184, 0xb0},
	{OV7251_8BIT, 0x5185, 0xb0},
	{OV7251_8BIT, 0x370c, 0x0c},
	{OV7251_8BIT, 0x3035, 0x00},
	{OV7251_8BIT, 0x3036, 0x2c}, /* 422.4 MHz */
	{OV7251_8BIT, 0x3037, 0xa1},
	{OV7251_8BIT, 0x303e, 0x19},
	{OV7251_8BIT, 0x3038, 0x06},
	{OV7251_8BIT, 0x3018, 0x04},
	{OV7251_8BIT, 0x3000, 0x00}, /* added for power optimization */
	{OV7251_8BIT, 0x3001, 0x00},
	{OV7251_8BIT, 0x3002, 0x00},
	{OV7251_8BIT, 0x3a0f, 0x40},
	{OV7251_8BIT, 0x3a10, 0x38},
	{OV7251_8BIT, 0x3a1b, 0x48},
	{OV7251_8BIT, 0x3a1e, 0x30},
	{OV7251_8BIT, 0x3a11, 0x90},
	{OV7251_8BIT, 0x3a1f, 0x10},
	{OV7251_8BIT, 0x3503, 0x07}, /* manual 3a */
	{OV7251_8BIT, 0x3500, 0x00},
	{OV7251_8BIT, 0x3501, 0x3F},
	{OV7251_8BIT, 0x3502, 0x00},
	{OV7251_8BIT, 0x3508, 0x00},
	{OV7251_8BIT, 0x3509, 0x00},
	{OV7251_TOK_TERM, 0, 0}
};

static struct ov7251_reg const ov7251_720p_30fps[] = {
	{OV7251_8BIT, 0x3021, 0x03},
	{OV7251_8BIT, 0x3718, 0x10},
	{OV7251_8BIT, 0x3702, 0x24},
	{OV7251_8BIT, 0x373a, 0x60},
	{OV7251_8BIT, 0x3715, 0x01},
	{OV7251_8BIT, 0x3703, 0x2e},
	{OV7251_8BIT, 0x3705, 0x10},
	{OV7251_8BIT, 0x3730, 0x30},
	{OV7251_8BIT, 0x3704, 0x62},
	{OV7251_8BIT, 0x3f06, 0x3a},
	{OV7251_8BIT, 0x371c, 0x00},
	{OV7251_8BIT, 0x371d, 0xc4},
	{OV7251_8BIT, 0x371e, 0x01},
	{OV7251_8BIT, 0x371f, 0x0d},
	{OV7251_8BIT, 0x3708, 0x61},
	{OV7251_8BIT, 0x3709, 0x12},
	{OV7251_8BIT, 0x3800, 0x01},
	{OV7251_8BIT, 0x3801, 0x40}, /* H crop start: 320 */
	{OV7251_8BIT, 0x3802, 0x00},
	{OV7251_8BIT, 0x3803, 0xb1}, /* V crop start: 177 */
	{OV7251_8BIT, 0x3804, 0x06},
	{OV7251_8BIT, 0x3805, 0x55}, /* H crop end: 1621 */
	{OV7251_8BIT, 0x3806, 0x03},
	{OV7251_8BIT, 0x3807, 0x95}, /* V crop end: 918 */
	{OV7251_8BIT, 0x3808, 0x05},
	{OV7251_8BIT, 0x3809, 0x10}, /* H output size: 0x0788==1928 */
	{OV7251_8BIT, 0x380a, 0x02},
	{OV7251_8BIT, 0x380b, 0xe0}, /* output size: 0x02DE==734 */
	{OV7251_8BIT, 0x380c, 0x08},
	{OV7251_8BIT, 0x380d, 0x00}, /* H timing: 2048 */
	{OV7251_8BIT, 0x380e, 0x04},
	{OV7251_8BIT, 0x380f, 0xa3}, /* V timing: 1187 */
	{OV7251_8BIT, 0x3810, 0x00},
	{OV7251_8BIT, 0x3811, 0x03}, /* H window offset: 3 */
	{OV7251_8BIT, 0x3812, 0x00},
	{OV7251_8BIT, 0x3813, 0x02}, /* V window offset: 2 */
	{OV7251_8BIT, 0x3820, 0x80},
	{OV7251_8BIT, 0x3821, 0x06}, /* mirror */
	{OV7251_8BIT, 0x3814, 0x11},
	{OV7251_8BIT, 0x3815, 0x11},
	{OV7251_8BIT, 0x3612, 0x0b},
	{OV7251_8BIT, 0x3618, 0x04},
	{OV7251_8BIT, 0x3a08, 0x01},
	{OV7251_8BIT, 0x3a09, 0x50},
	{OV7251_8BIT, 0x3a0a, 0x01},
	{OV7251_8BIT, 0x3a0b, 0x18},
	{OV7251_8BIT, 0x3a0d, 0x03},
	{OV7251_8BIT, 0x3a0e, 0x03},
	{OV7251_8BIT, 0x4520, 0x00},
	{OV7251_8BIT, 0x4837, 0x1b},
	{OV7251_8BIT, 0x3600, 0x08},
	{OV7251_8BIT, 0x3621, 0xc0},
	{OV7251_8BIT, 0x3632, 0xd2}, /* added for power opt */
	{OV7251_8BIT, 0x3633, 0x23},
	{OV7251_8BIT, 0x3634, 0x54},
	{OV7251_8BIT, 0x3f01, 0x0c},
	{OV7251_8BIT, 0x5001, 0xc1},
	{OV7251_8BIT, 0x3614, 0xf0},
	{OV7251_8BIT, 0x3630, 0x2d},
	{OV7251_8BIT, 0x370b, 0x62},
	{OV7251_8BIT, 0x3706, 0x61},
	{OV7251_8BIT, 0x4000, 0x02},
	{OV7251_8BIT, 0x4002, 0xc5},
	{OV7251_8BIT, 0x4005, 0x08},
	{OV7251_8BIT, 0x404f, 0x84},
	{OV7251_8BIT, 0x4051, 0x00},
	{OV7251_8BIT, 0x5000, 0xcf}, /* manual 3a */
	{OV7251_8BIT, 0x301d, 0xf0}, /* enable group hold */
	{OV7251_8BIT, 0x3a18, 0x00},
	{OV7251_8BIT, 0x3a19, 0x80},
	{OV7251_8BIT, 0x3503, 0x07},
	{OV7251_8BIT, 0x4521, 0x00},
	{OV7251_8BIT, 0x5183, 0xb0},
	{OV7251_8BIT, 0x5184, 0xb0},
	{OV7251_8BIT, 0x5185, 0xb0},
	{OV7251_8BIT, 0x370c, 0x0c},
	{OV7251_8BIT, 0x3035, 0x00},
	{OV7251_8BIT, 0x3036, 0x26}, /* {0x3036, 0x2c}, //422.4 MHz */
	{OV7251_8BIT, 0x3037, 0xa1},
	{OV7251_8BIT, 0x303e, 0x19},
	{OV7251_8BIT, 0x3038, 0x06},
	{OV7251_8BIT, 0x3018, 0x04},
	{OV7251_8BIT, 0x3000, 0x00}, /* added for power optimization */
	{OV7251_8BIT, 0x3001, 0x00},
	{OV7251_8BIT, 0x3002, 0x00},
	{OV7251_8BIT, 0x3a0f, 0x40},
	{OV7251_8BIT, 0x3a10, 0x38},
	{OV7251_8BIT, 0x3a1b, 0x48},
	{OV7251_8BIT, 0x3a1e, 0x30},
	{OV7251_8BIT, 0x3a11, 0x90},
	{OV7251_8BIT, 0x3a1f, 0x10},
	{OV7251_8BIT, 0x3503, 0x07}, /* manual 3a */
	{OV7251_8BIT, 0x3500, 0x00},
	{OV7251_8BIT, 0x3501, 0x3F},
	{OV7251_8BIT, 0x3502, 0x00},
	{OV7251_8BIT, 0x3508, 0x00},
	{OV7251_8BIT, 0x3509, 0x00},
	{OV7251_TOK_TERM, 0, 0},
};

struct ov7251_resolution ov7251_res_preview[] = {
	{
		.desc = "ov7251_480P_30fps",
		.width = 640,
		.height = 480,
		.fps = 30,
		.pix_clk_freq = 73,
		.used = 0,
/*		.pixels_per_line = 2048,
//		.lines_per_frame = 1184, */
		.pixels_per_line = 928,
		.lines_per_frame = 1724,
		.bin_factor_x = 1,
		.bin_factor_y = 1,
		.bin_mode = 0,
		.skip_frames = 3,
		.regs = ov7251_480P_30fps,
	},

};
#define N_RES_PREVIEW (ARRAY_SIZE(ov7251_res_preview))

struct ov7251_resolution ov7251_res_still[] = {
	{
		.desc = "ov7251_480P_30fps",
		.width = 640,
		.height = 480,
		.fps = 30,
		.pix_clk_freq = 73,
		.used = 0,
/*		.pixels_per_line = 2048,
//		.lines_per_frame = 1184, */
		.pixels_per_line = 928,
		.lines_per_frame = 1724,
		.bin_factor_x = 1,
		.bin_factor_y = 1,
		.bin_mode = 0,
		.skip_frames = 3,
		.regs = ov7251_480P_30fps,
		.mipi_freq = 422400,
	},
};
#define N_RES_STILL (ARRAY_SIZE(ov7251_res_still))

struct ov7251_resolution ov7251_res_video[] = {
	{
		.desc = "ov7251_480P_30fps",
/*		.width = 652,
//		.height = 492, */
		.width = 640,
		.height = 480,
		.fps = 30,
		.pix_clk_freq = 73,
		.used = 0,
/*		.pixels_per_line = 2048,
//		.lines_per_frame = 1184, */
		.pixels_per_line = 928,
		.lines_per_frame = 1724,
		.bin_factor_x = 1,
		.bin_factor_y = 1,
		.bin_mode = 0,
		.skip_frames = 3,
		.regs = ov7251_480P_30fps,
	},
};
#define N_RES_VIDEO (ARRAY_SIZE(ov7251_res_video))

static struct ov7251_resolution *ov7251_res = ov7251_res_preview;
static int N_RES = N_RES_PREVIEW;
#endif
