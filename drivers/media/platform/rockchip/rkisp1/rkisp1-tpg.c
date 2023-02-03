/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Rockchip ISP1 Driver - Test pattern generator
 *
 * Copyright (C) 2019 Collabora, Ltd.
 * Copyright (C) 2023 Ideas on Board
 *
 * Based on Rockchip ISP1 driver by Rockchip Electronics Co., Ltd.
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#include <linux/container_of.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mutex.h>

#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "rkisp1-common.h"
#include "rkisp1-tpg.h"

#define RKISP1_TPG_DEV_NAME	RKISP1_DRIVER_NAME "_tpg"

/* Same as the ISP. */
#define RKISP1_TPG_DEF_FMT	MEDIA_BUS_FMT_SRGGB10_1X10

/*
 * This came from measuring the effects of RKISP1_CIF_ISP_TPG_TOTAL_IN on
 * the freerunning framerate. It also matches the max clock rate for the ISP
 * from the i.MX8MP datasheet.
 */
#define RKISP1_TPG_CLOCK_RATE	500000000

/*
 * These are assumed from the maximum pre-defined size.
 * TODO Validate these.
 */
#define RKISP1_TPG_MAX_WIDTH	4096
#define RKISP1_TPG_MAX_HEIGHT	3072

/*
 * These are arbitrarily defined. 32x32 (minimum of the ISP) was too small to
 * be able to control the frame rate.
 * TODO Even 240x240 seems too small. Find better minimums.
 */
#define RKISP1_TPG_MIN_WIDTH	32
#define RKISP1_TPG_MIN_HEIGHT	32

#define RKISP1_TPG_W_H(w, h) ((((w) & 0x3fff) << 14) | ((h) & 0x3fff))

/*
 * TODO Should we add Disabled? And put the TPG subdev in
 * between the CSI2 receiver and the ISP?
 */
/*
 * The random generator mode needs a seed, however it doesn't seem to affect
 * the generated pattern, and it is all zeros.
 * TODO Investigate further the random generator seed register.
 */
static const char * const rkisp1_tpg_test_pattern_menu[] = {
	"3x3 color block",
	"Color bar",
	"Gray bar",
	"Highlighted grid",
	"Random generator"
};

static const int rkisp1_tpg_test_pattern_val[] = {
	RKISP1_CIF_ISP_TPG_CTRL_IMG_3X3_COLOR_BLOCK,
	RKISP1_CIF_ISP_TPG_CTRL_IMG_COLOR_BAR,
	RKISP1_CIF_ISP_TPG_CTRL_IMG_GRAY_BAR,
	RKISP1_CIF_ISP_TPG_CTRL_IMG_HIGHLIGHT_GRID,
	RKISP1_CIF_ISP_TPG_CTRL_IMG_RAND,
};

static inline struct rkisp1_tpg *to_rkisp1_tpg(struct v4l2_subdev *sd)
{
	return container_of(sd, struct rkisp1_tpg, sd);
}

static u32 rkisp1_tpg_calc_frame_sync(struct v4l2_fract interval,
				      u32 width, u32 height)
{
	u32 b, c, tmp;

	b = width + height;

	tmp = mult_frac(RKISP1_TPG_CLOCK_RATE,
			interval.numerator, interval.denominator);
	c = (width * height) - tmp;

	tmp = -b + int_sqrt((b * b) - (4 * c));

	return tmp / 2;
}

static void rkisp1_tpg_config_regs(struct rkisp1_tpg *tpg)
{
	struct rkisp1_device *rkisp1 = tpg->rkisp1;
	const struct rkisp1_mbus_info *mbus_info;
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_subdev_state *sd_state;
	u32 sync;
	u32 val;
	u32 tpg_ctrl;

	sd_state = v4l2_subdev_lock_and_get_active_state(&tpg->sd);
	fmt = v4l2_subdev_get_pad_format(&tpg->sd, sd_state, 0);
	v4l2_subdev_unlock_state(sd_state);

	mbus_info = rkisp1_mbus_info_get_by_code(fmt->code);

	tpg_ctrl = RKISP1_CIF_ISP_TPG_CTRL_DEF_SYNC
		 | RKISP1_CIF_ISP_TPG_CTRL_MAX_SYNC;

	/* We don't need to validate the format as it is already done */

	/* The bayer_pat enum happens to be the same as the register */
	tpg_ctrl |= (mbus_info->bayer_pat) << 4;

	tpg_ctrl |= rkisp1_tpg_test_pattern_val[tpg->tp_ctrl->val];

	/* default cannot happen as it is filtered */
	switch (mbus_info->bus_width) {
	case 8:
	default:
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_DEPTH_8;
		break;
	case 10:
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_DEPTH_10;
		break;
	case 12:
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_DEPTH_12;
		break;
	}

	dev_dbg(rkisp1->dev, "%s: setting size to %dx%d\n", __func__,
		fmt->width, fmt->height);

	/*
	 * TODO: Get better fps comparators. Or we can just get rid of these
	 * built-in ones, as they only work for these specific fps values
	 * anyway.
	 */
	if (fmt->width == 1920 && fmt->height == 1080 &&
	    tpg->interval.numerator == 1 &&
	    tpg->interval.denominator == 89) {
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_SOL_1080P;
	} else if (fmt->width == 1280 && fmt->height == 720 &&
		   tpg->interval.numerator == 1 &&
		   tpg->interval.denominator == 89) {
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_SOL_720P;
	} else if (fmt->width == 3840 && fmt->height == 2160 &&
		   tpg->interval.numerator == 1 &&
		   tpg->interval.denominator == 34) {
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_SOL_4K;
	} else {
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_SOL_USER_DEFINED;
		tpg_ctrl &= ~(RKISP1_CIF_ISP_TPG_CTRL_DEF_SYNC |
			      RKISP1_CIF_ISP_TPG_CTRL_MAX_SYNC);

		val = RKISP1_TPG_W_H(fmt->width, fmt->height);
		rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_ACT_IN, val);

		sync = rkisp1_tpg_calc_frame_sync(tpg->interval,
						  fmt->width, fmt->height);

		val = RKISP1_TPG_W_H(fmt->width + sync,
				     fmt->height + sync);
		rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_TOTAL_IN, val);

		/*
		 * These seem to be fine as arbitrary values
		 * TODO: Figure out if these values can be improved.
		 */
		val = RKISP1_TPG_W_H(sync / 3, sync / 3);
		rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_FP_IN, val);
		rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_BP_IN, val);
		rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_W_IN, val);

		/* The size of one block in the 3x3 color block mode */
		val = RKISP1_TPG_W_H(fmt->width / 3, fmt->height / 3);
		rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_GAP_IN, val);

		/*
		 * The width of one column in color bar, gray bar, and
		 * highlighted grid modes.
		 */
		val = (fmt->width / 8) & 0x3fff;
		rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_GAP_STD_IN, val);
	}

	rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_CTRL, tpg_ctrl);

	val = rkisp1_read(rkisp1, RKISP1_CIF_ISP_TPG_CTRL);
	dev_dbg(rkisp1->dev, "%s: wrote to ctrl %x\n", __func__, val);
}

static void rkisp1_tpg_enable(struct rkisp1_tpg *tpg, bool enable)
{
	struct rkisp1_device *rkisp1 = tpg->rkisp1;
	u32 val;

	val = rkisp1_read(rkisp1, RKISP1_CIF_ISP_TPG_CTRL);

	if (!enable) {
		rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_CTRL,
			     val & ~RKISP1_CIF_ISP_TPG_CTRL_ENA);
		return;
	}

	rkisp1_write(rkisp1, RKISP1_CIF_ISP_TPG_CTRL,
		     val | RKISP1_CIF_ISP_TPG_CTRL_ENA);
}

static void rkisp1_tpg_start(struct rkisp1_tpg *tpg)
{
	rkisp1_tpg_config_regs(tpg);
	rkisp1_tpg_enable(tpg, 1);
}

static void rkisp1_tpg_stop(struct rkisp1_tpg *tpg)
{
	rkisp1_tpg_enable(tpg, 0);
}

static int rkisp1_tpg_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rkisp1_tpg *tpg =
		container_of(ctrl->handler, struct rkisp1_tpg, ctrl_handler);
	struct rkisp1_device *rkisp1 = tpg->rkisp1;
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		/*
		 * We can't write the register directly here as it will be lost
		 * when the ISP is powered off.
		 * TODO: Allow setting the test pattern at runtime. Save the
		 * control value, and set the register in the interrupt handler.
		 */
		break;
	default:
		dev_info(rkisp1->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops rkisp1_tpg_ctrl_ops = {
	.s_ctrl = rkisp1_tpg_set_ctrl,
};

static int rkisp1_tpg_init_controls(struct rkisp1_tpg *tpg)
{
	struct v4l2_ctrl_handler *ctrl_hdlr;
	int ret;

	ctrl_hdlr = &tpg->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 1);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &tpg->lock;

	tpg->tp_ctrl =
		v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &rkisp1_tpg_ctrl_ops,
					     V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(rkisp1_tpg_test_pattern_menu) - 1,
					     0, 0, rkisp1_tpg_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(tpg->rkisp1->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	tpg->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static void rkisp1_tpg_free_controls(struct rkisp1_tpg *tpg)
{
	v4l2_ctrl_handler_free(tpg->sd.ctrl_handler);
}

/* ----------------------------------------------------------------------------
 * Subdev pad operations
 */

static int rkisp1_tpg_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	unsigned int i;
	int pos = 0;

	for (i = 0; ; i++) {
		const struct rkisp1_mbus_info *fmt =
			rkisp1_mbus_info_get_by_index(i);

		if (!fmt)
			return -EINVAL;

		if (!(fmt->pixel_enc & V4L2_PIXEL_ENC_BAYER))
			continue;

		if (code->index == pos) {
			code->code = fmt->mbus_code;
			return 0;
		}

		pos++;
	}

	return -EINVAL;
}

static int rkisp1_tpg_enum_frame_size(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_frame_size_enum *fse)
{
	const struct rkisp1_mbus_info *fmt =
		rkisp1_mbus_info_get_by_code(fse->code);

	if (!fmt || !(fmt->pixel_enc & V4L2_PIXEL_ENC_BAYER) ||
	    fse->index != 0 || fse->pad != 0)
		return -EINVAL;

	fse->min_width = RKISP1_TPG_MIN_WIDTH;
	fse->max_width = RKISP1_TPG_MAX_WIDTH;
	fse->min_height = RKISP1_TPG_MIN_HEIGHT;
	fse->max_height = RKISP1_TPG_MAX_HEIGHT;

	return 0;
}

static int rkisp1_tpg_init_config(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state)
{
	struct rkisp1_tpg *tpg = to_rkisp1_tpg(sd);
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_get_pad_format(sd, sd_state, 0);

	fmt->width = RKISP1_DEFAULT_WIDTH;
	fmt->height = RKISP1_DEFAULT_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->code = RKISP1_TPG_DEF_FMT;

	tpg->interval = (struct v4l2_fract){
		.numerator = 1,
		.denominator = 30,
	};

	return 0;
}

static int rkisp1_tpg_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *fmt)
{
	struct rkisp1_tpg *tpg = to_rkisp1_tpg(sd);
	const struct rkisp1_mbus_info *mbus_info;
	struct v4l2_mbus_framefmt *src_fmt;

	mutex_lock(&tpg->lock);

	src_fmt = v4l2_subdev_get_pad_format(&tpg->sd, sd_state, 0);

	src_fmt->code = fmt->format.code;

	mbus_info = rkisp1_mbus_info_get_by_code(src_fmt->code);
	if (!mbus_info || !(mbus_info->pixel_enc & V4L2_PIXEL_ENC_BAYER)) {
		src_fmt->code = RKISP1_TPG_DEF_FMT;
		mbus_info = rkisp1_mbus_info_get_by_code(src_fmt->code);
	}

	/*
	 * We don't actually have documentation on the minimum and maximum
	 * sizes supported by the TPG. Assume an arbitrary minimum and 12MP
	 * maximum.
	 */
	src_fmt->width = clamp_t(u32, fmt->format.width,
				 RKISP1_TPG_MIN_WIDTH,
				 RKISP1_TPG_MAX_WIDTH);
	src_fmt->height = clamp_t(u32, fmt->format.height,
				  RKISP1_TPG_MIN_HEIGHT,
				  RKISP1_TPG_MAX_HEIGHT);

	fmt->format = *src_fmt;

	mutex_unlock(&tpg->lock);

	return 0;
}

/* ----------------------------------------------------------------------------
 * Subdev video operations
 */

static int rkisp1_tpg_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct rkisp1_tpg *tpg = to_rkisp1_tpg(sd);

	if (!enable) {
		rkisp1_tpg_stop(tpg);
		return 0;
	}

	rkisp1_tpg_start(tpg);

	return 0;
}

static int rkisp1_tpg_g_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_frame_interval *interval)
{
	struct rkisp1_tpg *tpg = to_rkisp1_tpg(sd);

	if (interval->pad != 0)
		return -EINVAL;

	interval->interval = tpg->interval;

	return 0;
}

static int rkisp1_tpg_s_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_frame_interval *interval)
{
	struct rkisp1_tpg *tpg = to_rkisp1_tpg(sd);
	u32 sync;
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_subdev_state *sd_state;

	sd_state = v4l2_subdev_lock_and_get_active_state(sd);
	fmt = v4l2_subdev_get_pad_format(&tpg->sd, sd_state, 0);
	v4l2_subdev_unlock_state(sd_state);

	if (interval->pad != 0)
		return -EINVAL;

	sync = rkisp1_tpg_calc_frame_sync(interval->interval,
					  fmt->width, fmt->height);

	/*
	 * TODO Come up with better frame interval validation. Or get rid of
	 * g/s_frame_interval and just use hblank/vblank. Check the ratio
	 * between active time and sync time?
	 * Data points:
	 * - 1080p max 210 min 2 fps
	 */
	interval->interval.numerator =
		(fmt->width + sync) * (fmt->height + sync);
	interval->interval.denominator = RKISP1_TPG_CLOCK_RATE;

	tpg->interval = interval->interval;

	return 0;
}

/* ----------------------------------------------------------------------------
 * Registration
 */

static const struct media_entity_operations rkisp1_tpg_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_video_ops rkisp1_tpg_video_ops = {
	.s_stream = rkisp1_tpg_s_stream,
	.g_frame_interval = rkisp1_tpg_g_frame_interval,
	.s_frame_interval = rkisp1_tpg_s_frame_interval,
};

static const struct v4l2_subdev_pad_ops rkisp1_tpg_pad_ops = {
	.enum_mbus_code = rkisp1_tpg_enum_mbus_code,
	.enum_frame_size = rkisp1_tpg_enum_frame_size,
	.init_cfg = rkisp1_tpg_init_config,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = rkisp1_tpg_set_fmt,
};

static const struct v4l2_subdev_ops rkisp1_tpg_ops = {
	.video = &rkisp1_tpg_video_ops,
	.pad = &rkisp1_tpg_pad_ops,
};

int rkisp1_tpg_register(struct rkisp1_device *rkisp1)
{
	struct rkisp1_tpg *tpg = &rkisp1->tpg;
	struct media_pad *pad;
	struct v4l2_subdev *sd;
	int ret;

	tpg->rkisp1 = rkisp1;
	mutex_init(&tpg->lock);

	ret = rkisp1_tpg_init_controls(tpg);
	if (ret)
		goto error;

	sd = &tpg->sd;
	v4l2_subdev_init(sd, &rkisp1_tpg_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.ops = &rkisp1_tpg_media_ops;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sd->owner = THIS_MODULE;
	strscpy(sd->name, RKISP1_TPG_DEV_NAME, sizeof(sd->name));

	pad = &tpg->pad;
	pad->flags = MEDIA_PAD_FL_SOURCE | MEDIA_PAD_FL_MUST_CONNECT;

	ret = media_entity_pads_init(&sd->entity, 1, pad);
	if (ret) {
		dev_err(rkisp1->dev, "Failed to initialize media entity pads\n");
		goto err_controls_cleanup;
	}

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_entity_cleanup;

	ret = v4l2_device_register_subdev(&tpg->rkisp1->v4l2_dev, sd);
	if (ret) {
		dev_err(rkisp1->dev, "Failed to register tpg subdev\n");
		goto err_subdev_cleanup;
	}

	return 0;

err_subdev_cleanup:
	v4l2_subdev_cleanup(sd);
err_entity_cleanup:
	media_entity_cleanup(&sd->entity);
err_controls_cleanup:
	rkisp1_tpg_free_controls(tpg);
error:
	mutex_destroy(&tpg->lock);
	tpg->rkisp1 = NULL;
	return ret;
}

void rkisp1_tpg_unregister(struct rkisp1_device *rkisp1)
{
	struct rkisp1_tpg *tpg = &rkisp1->tpg;

	if (!tpg->rkisp1)
		return;

	v4l2_device_unregister_subdev(&tpg->sd);
	v4l2_subdev_cleanup(&tpg->sd);
	media_entity_cleanup(&tpg->sd.entity);
	rkisp1_tpg_free_controls(tpg);
	mutex_destroy(&tpg->lock);
}
