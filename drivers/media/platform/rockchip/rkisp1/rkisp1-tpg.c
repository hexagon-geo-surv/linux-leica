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
#include <linux/minmax.h>
#include <linux/mutex.h>

#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "rkisp1-common.h"
#include "rkisp1-tpg.h"

#define RKISP1_TPG_DEV_NAME	RKISP1_DRIVER_NAME "_tpg"

/* Same as the ISP. */
#define RKISP1_TPG_DEF_FMT	MEDIA_BUS_FMT_SRGGB10_1X10

static inline struct rkisp1_tpg *to_rkisp1_tpg(struct v4l2_subdev *sd)
{
	return container_of(sd, struct rkisp1_tpg, sd);
}

static struct v4l2_mbus_framefmt *
rkisp1_tpg_get_pad_fmt(struct rkisp1_tpg *tpg,
		       struct v4l2_subdev_state *sd_state,
		       unsigned int pad, u32 which)
{
	struct v4l2_subdev_state state = {
		.pads = &tpg->pad_cfg
	};

	lockdep_assert_held(&tpg->lock);

	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_format(&tpg->sd, sd_state, pad);
	else
		return v4l2_subdev_get_try_format(&tpg->sd, &state, pad);
}

static void rkisp1_tpg_config_regs(struct rkisp1_tpg *tpg)
{
	struct rkisp1_device *rkisp1 = tpg->rkisp1;
	const struct rkisp1_mbus_info *mbus_info = tpg->src_fmt;
	u32 val;
	u32 tpg_ctrl;

	tpg_ctrl = RKISP1_CIF_ISP_TPG_CTRL_DEF_SYNC
		 | RKISP1_CIF_ISP_TPG_CTRL_MAX_SYNC;

	/* We don't need to validate the format as it is already done */

	/* The bayer_pat enum happens to be the same as the register */
	tpg_ctrl |= (mbus_info->bayer_pat) << 4;

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

	/* TODO Support other resolutions */
	if (tpg->src_width == 1920 && tpg->src_height == 1080) {
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_SOL_1080P;
	} else if (tpg->src_width == 1280 && tpg->src_height == 720) {
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_SOL_720P;
	} else {
		dev_err(rkisp1->dev,
			"Unsupported resolution %dx%d, defaulting to 1080P\n",
			tpg->src_width, tpg->src_height);
		tpg_ctrl |= RKISP1_CIF_ISP_TPG_CTRL_SOL_1080P;
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

static int rkisp1_tpg_init_config(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_get_try_format(sd, sd_state, 0);

	/*
	 * As we don't have documentation for the TPG component, we'll stick to
	 * using a pre-defined resolution for now, just to get TPG working.
	 * TODO Support other resolutions
	 */
	fmt->width = 1920;
	fmt->height = 1080;
	fmt->field = V4L2_FIELD_NONE;
	fmt->code = RKISP1_TPG_DEF_FMT;

	return 0;
}

static int rkisp1_tpg_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *fmt)
{
	struct rkisp1_tpg *tpg = to_rkisp1_tpg(sd);

	mutex_lock(&tpg->lock);
	fmt->format = *rkisp1_tpg_get_pad_fmt(tpg, sd_state, fmt->pad,
					      fmt->which);
	mutex_unlock(&tpg->lock);

	return 0;
}

static int rkisp1_tpg_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *fmt)
{
	struct rkisp1_tpg *tpg = to_rkisp1_tpg(sd);
	const struct rkisp1_mbus_info *mbus_info;
	struct v4l2_mbus_framefmt *src_fmt;
	static const u32 area_1080p = 1920 * 1080;
	static const u32 area_720p = 1280 * 720;
	u32 area;

	mutex_lock(&tpg->lock);

	src_fmt = rkisp1_tpg_get_pad_fmt(tpg, sd_state, 0, fmt->which);

	src_fmt->code = fmt->format.code;

	mbus_info = rkisp1_mbus_info_get_by_code(src_fmt->code);
	if (!mbus_info || !(mbus_info->pixel_enc & V4L2_PIXEL_ENC_BAYER)) {
		src_fmt->code = RKISP1_TPG_DEF_FMT;
		mbus_info = rkisp1_mbus_info_get_by_code(src_fmt->code);
	}

	/*
	 * Quick hack to clamp to either of these two resolutions. This will be
	 * replaced with proper clamping once we support arbitrary resolutions.
	 * TODO Support other resolutions
	 */
	area = clamp_t(u32, fmt->format.width * fmt->format.height,
		       area_720p, area_1080p);
	if (area - area_720p < area - area_1080p) {
		src_fmt->width = 1280;
		src_fmt->height = 720;
	} else {
		src_fmt->width = 1920;
		src_fmt->height = 1080;
	}

	fmt->format = *src_fmt;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		tpg->src_fmt = mbus_info;
		tpg->src_width = fmt->format.width;
		tpg->src_height = fmt->format.height;
	}

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

/* ----------------------------------------------------------------------------
 * Registration
 */

static const struct media_entity_operations rkisp1_tpg_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_video_ops rkisp1_tpg_video_ops = {
	.s_stream = rkisp1_tpg_s_stream,
};

static const struct v4l2_subdev_pad_ops rkisp1_tpg_pad_ops = {
	.enum_mbus_code = rkisp1_tpg_enum_mbus_code,
	.init_cfg = rkisp1_tpg_init_config,
	.get_fmt = rkisp1_tpg_get_fmt,
	.set_fmt = rkisp1_tpg_set_fmt,
};

static const struct v4l2_subdev_ops rkisp1_tpg_ops = {
	.video = &rkisp1_tpg_video_ops,
	.pad = &rkisp1_tpg_pad_ops,
};

int rkisp1_tpg_register(struct rkisp1_device *rkisp1)
{
	struct rkisp1_tpg *tpg = &rkisp1->tpg;
	struct v4l2_subdev_state state = {};
	struct media_pad *pad;
	struct v4l2_subdev *sd;
	int ret;

	tpg->rkisp1 = rkisp1;
	mutex_init(&tpg->lock);

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
		goto error;
	}

	state.pads = &tpg->pad_cfg;
	rkisp1_tpg_init_config(sd, &state);

	ret = v4l2_device_register_subdev(&tpg->rkisp1->v4l2_dev, sd);
	if (ret) {
		dev_err(rkisp1->dev, "Failed to register tpg subdev\n");
		goto error;
	}

	return 0;

error:
	media_entity_cleanup(&sd->entity);
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
	media_entity_cleanup(&tpg->sd.entity);
	mutex_destroy(&tpg->lock);
}
