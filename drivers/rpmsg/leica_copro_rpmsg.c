// SPDX-License-Identifier: GPL-2.0
//
// Copyright Leica Geosystems AG.

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>

#include "rpmsg_char.h"

static int leica_copro_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct rpmsg_channel_info chinfo;
	struct device *dev = &rpdev->dev;
	int ret;

	memcpy(chinfo.name, rpdev->id.name, RPMSG_NAME_SIZE);
	chinfo.src = rpdev->src;
	chinfo.dst = rpdev->dst;

	return rpmsg_chrdev_eptdev_create(rpdev, dev, chinfo);
}

static void leica_copro_rpmsg_remove(struct rpmsg_device *rpdev)
{
	int ret;

	ret = device_for_each_child(&rpdev->dev, NULL, rpmsg_chrdev_eptdev_destroy);
	if (ret)
		dev_warn(&rpdev->dev, "failed to destroy endpoints: %d\n", ret);
}

static struct rpmsg_device_id leica_copro_rpmsg_id_table[] = {
	{ .name = "tps-copro-angle" },
	{ .name = "tps-copro-motorization" },
	{ .name = "tps-copro-knob" },
	{ .name = "tps-copro-service" },
	{ .name = "tps-copro-logging" },
	{ .name = "tps-copro-tunnel-hz" },
	{ .name = "tps-copro-tunnel-v" },
	{ .name = "tps-copro-streaming" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, leica_copro_rpmsg_id_table);

static struct rpmsg_driver leica_copro_rpmsg_id = {
	.drv.name = KBUILD_MODNAME,
	.id_table = leica_copro_rpmsg_id_table,
	.probe = leica_copro_rpmsg_probe,
	.remove = leica_copro_rpmsg_remove,
};
module_rpmsg_driver(leica_copro_rpmsg_id);

MODULE_DESCRIPTION("Leica CoPro RPMSG driver");
MODULE_LICENSE("GPL v2");
