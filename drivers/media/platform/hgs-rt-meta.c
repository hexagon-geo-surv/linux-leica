// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#include <linux/leica-hif-mipi-tx.h>

#define DRIVER_NAME			   "hgs-rt-meta"

#define LEICA_RT_META_DATA_MAGIC	   0x47110815

#define LEICA_RT_META_DATA_CMD_HELO	   1
#define LEICA_RT_META_DATA_CMD_FRAMESYNC   2
#define LEICA_RT_META_DATA_CMD_START_PULSE 3
#define LEICA_RT_META_DATA_CMD_STOP_PULSE  4

struct leica_rt_meta_data {
	uint32_t magic;
	uint32_t cmd;

	union {
		struct {
			uint64_t cnt;
			uint64_t ts;
		} framesync;
	} data;
} __attribute__((packed));

enum rt_meta_pads {
	RT_META_PAD_SINK = 0,
	RT_META_PAD_SOURCE = 1,
	RT_META_PAD_COUNT,
};

struct rt_meta {
	struct device *dev;
	struct mutex mutex;
	struct v4l2_fwnode_endpoint vep;
	const char *name;
	bool generate_syncpulse;
	struct gpio_desc *dbg_gpio;

	struct v4l2_subdev subdev;
	struct v4l2_async_notifier notifier;
	struct media_pad pads[RT_META_PAD_COUNT];

	struct v4l2_mbus_framefmt mbus_fmt;

	struct v4l2_subdev *source_subdev;
	int source_pad;

	struct v4l2_ctrl_handler ctrls;

	struct rpmsg_driver rpmsg_driver;
	struct rpmsg_device *rpdev;

	bool cnt_offset_trigger;
	u64 cnt_offset;
};

static const u32 mbus_fmt_infos[] = {
	/* Raw RGGB (bayer) */
	MEDIA_BUS_FMT_SRGGB8_1X8,
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SRGGB12_1X12,
	MEDIA_BUS_FMT_SRGGB14_1X14,
	MEDIA_BUS_FMT_SRGGB16_1X16,
	/* Raw GRBG (bayer) */
	MEDIA_BUS_FMT_SGRBG8_1X8,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGRBG12_1X12,
	MEDIA_BUS_FMT_SGRBG14_1X14,
	MEDIA_BUS_FMT_SGRBG16_1X16,
	/* Raw BGGR (bayer) */
	MEDIA_BUS_FMT_SBGGR8_1X8,
	MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_BE,
	MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_LE,
	MEDIA_BUS_FMT_SBGGR10_2X8_PADLO_BE,
	MEDIA_BUS_FMT_SBGGR10_2X8_PADLO_LE,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SBGGR12_1X12,
	MEDIA_BUS_FMT_SBGGR14_1X14,
	MEDIA_BUS_FMT_SBGGR16_1X16,
	/* Raw GBRG (bayer) */
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGBRG12_1X12,
	MEDIA_BUS_FMT_SGBRG14_1X14,
	MEDIA_BUS_FMT_SGBRG16_1X16,
	/* Gray (monochrome) */
	MEDIA_BUS_FMT_Y8_1X8,
	MEDIA_BUS_FMT_Y10_1X10,
	MEDIA_BUS_FMT_Y10_2X8_PADHI_LE,
	MEDIA_BUS_FMT_Y12_1X12,
	MEDIA_BUS_FMT_Y14_1X14,
	MEDIA_BUS_FMT_Y16_1X16,
};

static bool rt_meta_is_mbus_code_supported(u32 code)
{
	for (int i = 0; i < ARRAY_SIZE(mbus_fmt_infos); i++) {
		if (code == mbus_fmt_infos[i])
			return true;
	}

	return false;
}

static_assert(sizeof(struct leica_hif_mipi_tx_event) <= sizeof(((struct v4l2_event *)0)->u.data));

static int rt_meta_start(struct rt_meta *rt_meta)
{
	int ret;

	rt_meta->cnt_offset_trigger = true;

	if (rt_meta->source_subdev) {
		ret = v4l2_subdev_call(rt_meta->source_subdev, video, s_stream, true);
		if (ret) {
			dev_warn(rt_meta->dev, "Failed to start source subdev\n");
			goto err_exit;
		}
	}

	if (rt_meta->generate_syncpulse && rt_meta->rpdev) {
		struct leica_rt_meta_data meta_data = { 0 };

		meta_data.magic = LEICA_RT_META_DATA_MAGIC;
		meta_data.cmd = LEICA_RT_META_DATA_CMD_START_PULSE;

		ret = rpmsg_trysend(rt_meta->rpdev->ept, (void *)&meta_data, sizeof(meta_data));
		if (ret)
			dev_err(rt_meta->dev, "Failed to send message to remote\n");
	}

	return 0;

err_exit:
	return ret;
}

static int rt_meta_stop(struct rt_meta *rt_meta)
{
	if (rt_meta->generate_syncpulse && rt_meta->rpdev) {
		struct leica_rt_meta_data meta_data = { 0 };
		int ret;

		meta_data.magic = LEICA_RT_META_DATA_MAGIC;
		meta_data.cmd = LEICA_RT_META_DATA_CMD_STOP_PULSE;

		ret = rpmsg_trysend(rt_meta->rpdev->ept, (void *)&meta_data, sizeof(meta_data));
		if (ret)
			dev_err(rt_meta->dev, "Failed to send message to remote\n");
	}

	if (rt_meta->source_subdev) {
		if (v4l2_subdev_call(rt_meta->source_subdev, video, s_stream, false))
			dev_warn(rt_meta->dev, "Failed to stop source subdev\n");
	}

	return 0;
}

static inline struct rt_meta *v4l2_subdev_to_rt_meta(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct rt_meta, subdev);
}

static int rt_meta_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct rt_meta *rt_meta = v4l2_subdev_to_rt_meta(subdev);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(subdev);

	if (enable) {
		ret = rt_meta_start(rt_meta);
		if (ret)
			goto unlock;

	} else {
		rt_meta_stop(rt_meta);
	}

unlock:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static int rt_meta_link_setup(struct media_entity *entity, const struct media_pad *local,
			      const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *subdev = media_entity_to_v4l2_subdev(entity);
	struct rt_meta *rt_meta = v4l2_subdev_to_rt_meta(subdev);
	struct v4l2_subdev *remote_sd;
	int ret = 0;

	remote_sd = media_entity_to_v4l2_subdev(remote->entity);

	mutex_lock(&rt_meta->mutex);

	if (local->flags & MEDIA_PAD_FL_SINK) {
		if (flags & MEDIA_LNK_FL_ENABLED) {
			if (rt_meta->source_subdev) {
				ret = -EBUSY;
				goto unlock;
			}
			rt_meta->source_subdev = remote_sd;
		} else {
			rt_meta->source_subdev = NULL;
		}
	}

unlock:
	mutex_unlock(&rt_meta->mutex);

	return ret;
}

static int rt_meta_pad_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(mbus_fmt_infos))
		return -EINVAL;

	code->code = mbus_fmt_infos[code->index];

	return 0;
}

static int rt_meta_get_pad_format(struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_format *fmt)
{
	struct rt_meta *rt_meta = v4l2_subdev_to_rt_meta(sd);
	struct v4l2_mbus_framefmt *framefmt;

	if (fmt->pad >= RT_META_PAD_COUNT)
		return -EINVAL;

	mutex_lock(&rt_meta->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
	else
		framefmt = &rt_meta->mbus_fmt;

	fmt->format = *framefmt;

	mutex_unlock(&rt_meta->mutex);

	return 0;
}

static void rt_meta_mbus_fmt_fill(struct v4l2_mbus_framefmt *mbus_fmt, u32 width, u32 height,
				  u32 code)
{
	if (!rt_meta_is_mbus_code_supported(code))
		code = mbus_fmt_infos[0];

	mbus_fmt->width = width;
	mbus_fmt->height = height;
	mbus_fmt->code = code;
	mbus_fmt->field = V4L2_FIELD_NONE;
	mbus_fmt->colorspace = V4L2_COLORSPACE_RAW;
	mbus_fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	mbus_fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	mbus_fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int rt_meta_set_pad_format(struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_format *fmt)
{
	struct rt_meta *rt_meta = v4l2_subdev_to_rt_meta(sd);
	struct v4l2_mbus_framefmt *framefmt;

	if (fmt->pad >= RT_META_PAD_COUNT)
		return -EINVAL;

	mutex_lock(&rt_meta->mutex);

	rt_meta_mbus_fmt_fill(&fmt->format, fmt->format.width, fmt->format.height,
			      fmt->format.code);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
	else
		framefmt = &rt_meta->mbus_fmt;

	*framefmt = fmt->format;

	/* Propagate the format from the sink to the source also in the try case. */
	if (fmt->pad == RT_META_PAD_SINK && fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_state_get_format(sd_state, RT_META_PAD_SOURCE);
		*framefmt = fmt->format;
	}

	mutex_unlock(&rt_meta->mutex);

	return 0;
}

static const struct media_entity_operations rt_meta_entity_ops = {
	.link_setup = rt_meta_link_setup,
	.link_validate = v4l2_subdev_link_validate,
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
};

static const struct v4l2_subdev_video_ops rt_meta_video_ops = {
	.s_stream = rt_meta_s_stream,
};

static const struct v4l2_subdev_pad_ops rt_meta_pad_ops = {
	.enum_mbus_code = rt_meta_pad_enum_mbus_code,
	.get_fmt = rt_meta_get_pad_format,
	.set_fmt = rt_meta_set_pad_format,
};

static int rt_meta_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
				   struct v4l2_event_subscription *sub)
{
	if (sub->type != LEICA_HIF_MIPI_TX_EVENT_META || sub->id != 0)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 16, NULL);
}

static const struct v4l2_subdev_core_ops rt_meta_core_ops = {
	.subscribe_event = rt_meta_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops rt_meta_subdev_ops = {
	.core = &rt_meta_core_ops,
	.video = &rt_meta_video_ops,
	.pad = &rt_meta_pad_ops,
};

static int rt_meta_async_bound(struct v4l2_async_notifier *notifier, struct v4l2_subdev *subdev,
			       struct v4l2_async_connection *asc)
{
	struct rt_meta *rt_meta = v4l2_subdev_to_rt_meta(notifier->sd);
	struct media_pad *sink = &rt_meta->subdev.entity.pads[RT_META_PAD_SINK];

	return v4l2_create_fwnode_links_to_pad(subdev, sink, 0);
}

static const struct v4l2_async_notifier_operations rt_meta_notifier_ops = {
	.bound = rt_meta_async_bound,
};

static int rt_meta_init_ctrls(struct rt_meta *rt_meta)
{
	struct v4l2_ctrl *ctrl;
	u64 *link_frequencies = rt_meta->vep.link_frequencies;

	v4l2_ctrl_handler_init(&rt_meta->ctrls, 1);

	ctrl = v4l2_ctrl_new_int_menu(&rt_meta->ctrls, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				      link_frequencies);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (rt_meta->ctrls.error) {
		dev_err(rt_meta->dev, "Failed to add controls (%d)\n", rt_meta->ctrls.error);
		return rt_meta->ctrls.error;
	}

	rt_meta->subdev.ctrl_handler = &rt_meta->ctrls;

	return 0;
}

static int rt_meta_subdev_init(struct rt_meta *rt_meta)
{
	int ret;

	v4l2_subdev_init(&rt_meta->subdev, &rt_meta_subdev_ops);
	rt_meta->subdev.entity.ops = &rt_meta_entity_ops;

	ret = rt_meta_init_ctrls(rt_meta);
	if (ret)
		return ret;

	rt_meta->subdev.owner = THIS_MODULE;
	snprintf(rt_meta->subdev.name, sizeof(rt_meta->subdev.name), "%s-%s", DRIVER_NAME,
		 rt_meta->name);
	rt_meta->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	rt_meta->subdev.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	rt_meta->subdev.dev = rt_meta->dev;

	rt_meta->pads[RT_META_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	rt_meta->pads[RT_META_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&rt_meta->subdev.entity, ARRAY_SIZE(rt_meta->pads),
				     rt_meta->pads);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(&rt_meta->subdev);
	if (ret) {
		media_entity_cleanup(&rt_meta->subdev.entity);
		return ret;
	}

	return 0;
}

static int rt_meta_async_register(struct rt_meta *rt_meta)
{
	struct v4l2_fwnode_endpoint v4l2_ep = { .bus_type = 0 };
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep_fwh;
	int ret;

	v4l2_async_subdev_nf_init(&rt_meta->notifier, &rt_meta->subdev);

	ep_fwh = fwnode_graph_get_endpoint_by_id(dev_fwnode(rt_meta->dev), 0, 0,
						 FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep_fwh)
		return -ENOTCONN;

	ret = v4l2_fwnode_endpoint_parse(ep_fwh, &v4l2_ep);
	if (ret) {
		dev_err(rt_meta->dev, "Could not parse v4l2 endpoint\n");
		goto err_parse;
	}

	asc = v4l2_async_nf_add_fwnode_remote(&rt_meta->notifier, ep_fwh,
					      struct v4l2_async_connection);
	if (IS_ERR(asc)) {
		ret = PTR_ERR(asc);
		goto err_parse;
	}

	fwnode_handle_put(ep_fwh);

	rt_meta->notifier.ops = &rt_meta_notifier_ops;

	ret = v4l2_async_nf_register(&rt_meta->notifier);
	if (ret)
		return ret;

	return v4l2_async_register_subdev(&rt_meta->subdev);

err_parse:
	fwnode_handle_put(ep_fwh);

	return ret;
}

static int rt_meta_parse_dt(struct rt_meta *rt_meta)
{
	struct device_node *np = dev_of_node(rt_meta->dev);
	struct fwnode_handle *endpoint;
	const char *label;
	int ret;

	if (!np) {
		dev_err(rt_meta->dev, "Missing OF node\n");
		return -EINVAL;
	}

	ret = of_property_read_string(np, "label", &label);
	if (ret)
		return dev_err_probe(rt_meta->dev, ret, "Missing or invalid property label\n");

	rt_meta->name = label;
	rt_meta->generate_syncpulse = of_property_read_bool(np, "generate-syncpulse");

	endpoint =
		fwnode_graph_get_endpoint_by_id(dev_fwnode(rt_meta->dev), RT_META_PAD_SINK, 0, 0);

	if (!endpoint)
		return dev_err_probe(rt_meta->dev, -EINVAL, "missing endpoint node\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &rt_meta->vep);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(rt_meta->dev, ret, "parsing endpoint node failed\n");

	rt_meta->dbg_gpio = devm_gpiod_get_optional(rt_meta->dev, "debug", GPIOD_OUT_LOW);
	if (IS_ERR(rt_meta->dbg_gpio))
		return dev_err_probe(rt_meta->dev, PTR_ERR(rt_meta->dbg_gpio),
				     "failed to get debug gpio\n");

	return ret;
}

#define rpdrv_to_rt_meta(__d)  container_of(__d, struct rt_meta, rpmsg_driver)
#define to_rpmsg_driver(__drv) container_of(__drv, struct rpmsg_driver, drv)

static int rt_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct rpmsg_driver *rpdrv = to_rpmsg_driver(rpdev->dev.driver);
	struct rt_meta *rt_meta = rpdrv_to_rt_meta(rpdrv);
	int ret;

	rt_meta->rpdev = rpdev;

	if (rt_meta->rpdev) {
		struct leica_rt_meta_data meta_data = { 0 };

		meta_data.magic = LEICA_RT_META_DATA_MAGIC;
		meta_data.cmd = LEICA_RT_META_DATA_CMD_HELO;

		ret = rpmsg_trysend(rt_meta->rpdev->ept, (void *)&meta_data, sizeof(meta_data));
		if (ret)
			dev_err(rt_meta->dev, "Failed to send message to remote\n");
	}

	return 0;
}

static void rt_rpmsg_remove(struct rpmsg_device *rpdev)
{
}

static int rt_rpmsg_callback(struct rpmsg_device *rpdev, void *buf, int len, void *priv, u32 addr)
{
	struct rpmsg_driver *rpdrv = to_rpmsg_driver(rpdev->dev.driver);
	struct rt_meta *rt_meta = rpdrv_to_rt_meta(rpdrv);
	struct v4l2_event v4l2_event = {
		.type = LEICA_HIF_MIPI_TX_EVENT_META,
	};
	struct leica_hif_mipi_tx_event *mipitx_event = (void *)&v4l2_event.u.data;
	const struct leica_rt_meta_data *meta_data = buf;

	if (len != sizeof(struct leica_rt_meta_data) ||
	    meta_data->magic != LEICA_RT_META_DATA_MAGIC) {
		dev_warn(rt_meta->dev, "Unexpected data\n");
		return 0;
	}

	switch (meta_data->cmd) {
	case LEICA_RT_META_DATA_CMD_FRAMESYNC:

		mipitx_event->meta.mipi_timestamp = meta_data->data.framesync.ts;
		if (rt_meta->cnt_offset_trigger) {
			rt_meta->cnt_offset = meta_data->data.framesync.cnt;
			rt_meta->cnt_offset_trigger = false;
		}
		mipitx_event->meta.mipi_framecounter =
			meta_data->data.framesync.cnt - rt_meta->cnt_offset;

		gpiod_set_value_cansleep(rt_meta->dbg_gpio, 1);
		usleep_range(20, 40);
		gpiod_set_value_cansleep(rt_meta->dbg_gpio, 0);

		v4l2_event_queue(rt_meta->subdev.devnode, &v4l2_event);
		break;

	default:
		dev_warn(rt_meta->dev, "Unknown command (%u)\n", meta_data->cmd);
		break;
	}

	return 0;
}

static const struct rpmsg_device_id rt_rpmsg_id_table[] = {
	{ .name = "rpmsg-rt-meta" },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(rpmsg, rt_rpmsg_id_table);

static struct rpmsg_driver rt_rpmsg_driver = {
	.probe = rt_rpmsg_probe,
	.remove = rt_rpmsg_remove,
	.callback = rt_rpmsg_callback,
	.id_table = rt_rpmsg_id_table,
	.drv.name = "rt_rpmsg_channel",
};

static int rt_meta_probe(struct platform_device *pdev)
{
	struct rt_meta *rt_meta;
	int ret;

	rt_meta = devm_kzalloc(&pdev->dev, sizeof(*rt_meta), GFP_KERNEL);
	if (!rt_meta)
		return -ENOMEM;

	rt_meta->dev = &pdev->dev;

	ret = rt_meta_parse_dt(rt_meta);
	if (ret)
		goto err_fwnode;

	platform_set_drvdata(pdev, &rt_meta->subdev);
	mutex_init(&rt_meta->mutex);
	rt_meta_mbus_fmt_fill(&rt_meta->mbus_fmt, 16, 16, 0);

	ret = rt_meta_subdev_init(rt_meta);
	if (ret < 0)
		goto err_mutex;

	ret = rt_meta_async_register(rt_meta);
	if (ret < 0)
		goto err_subdev;

	rt_meta->rpmsg_driver = rt_rpmsg_driver;
	ret = register_rpmsg_driver(&rt_meta->rpmsg_driver);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register RPMsg driver\n");
		goto err_async;
	}

	return 0;

err_async:
	v4l2_async_nf_unregister(&rt_meta->notifier);
	v4l2_async_nf_cleanup(&rt_meta->notifier);
	v4l2_async_unregister_subdev(&rt_meta->subdev);
err_subdev:
	v4l2_ctrl_handler_free(&rt_meta->ctrls);
	media_entity_cleanup(&rt_meta->subdev.entity);
	v4l2_subdev_cleanup(&rt_meta->subdev);
err_mutex:
	mutex_destroy(&rt_meta->mutex);
err_fwnode:
	v4l2_fwnode_endpoint_free(&rt_meta->vep);
	return ret;
}

static void rt_meta_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct rt_meta *rt_meta = v4l2_subdev_to_rt_meta(sd);

	unregister_rpmsg_driver(&rt_meta->rpmsg_driver);
	v4l2_async_nf_unregister(&rt_meta->notifier);
	v4l2_async_nf_cleanup(&rt_meta->notifier);
	v4l2_async_unregister_subdev(&rt_meta->subdev);
	v4l2_ctrl_handler_free(&rt_meta->ctrls);
	media_entity_cleanup(&rt_meta->subdev.entity);
	v4l2_subdev_cleanup(&rt_meta->subdev);
	v4l2_fwnode_endpoint_free(&rt_meta->vep);
	mutex_destroy(&rt_meta->mutex);
}

static const struct of_device_id rt_meta_of_match[] = {
	{ .compatible = "hgs,rt-meta" },
	{},
};
MODULE_DEVICE_TABLE(of, rt_meta_of_match);

static struct platform_driver rt_meta_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = rt_meta_of_match,
	},
	.probe = rt_meta_probe,
	.remove = rt_meta_remove,
};
module_platform_driver(rt_meta_platform_driver);

MODULE_AUTHOR("Matthias Fend <matthias.fend@emfend.at>");
MODULE_DESCRIPTION("Leica rpmsg realtime metadata v42l bridge");
MODULE_LICENSE("GPL v2");
