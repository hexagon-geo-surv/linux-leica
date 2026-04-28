/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Leica HIF MIPI-TX meta data v4l2 event
 *
 * Copyright Leica Geosystems AG
 *
 */
#ifndef _UAPI_LEICA_HIF_MIPI_TX_H_
#define _UAPI_LEICA_HIF_MIPI_TX_H_

#include <linux/types.h>

#define LEICA_HIF_MIPI_TX_EVENT_META (V4L2_EVENT_PRIVATE_START + 0)

struct leica_hif_mipi_tx_event_meta {
	__u32 hif_frame_id;
	__u32 hif_camera_id;
	__u32 hif_datatype;
	__u64 hif_timestamp;
	__u32 hif_exposure_time;
	__u32 hif_gain;
	__u64 hif_userdata;
	__u32 mipi_framecounter;
	__u64 mipi_timestamp;
};

struct leica_hif_mipi_tx_event {
	union {
		struct leica_hif_mipi_tx_event_meta meta;
	};
};

#endif
