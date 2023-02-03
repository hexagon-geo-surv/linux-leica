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
#ifndef _RKISP1_TPG_H
#define _RKISP1_TPG_H

struct rkisp1_device;

int rkisp1_tpg_register(struct rkisp1_device *rkisp1);
void rkisp1_tpg_unregister(struct rkisp1_device *rkisp1);

#endif /* _RKISP1_TPG_H */
