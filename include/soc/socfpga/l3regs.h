/*
 * Copyright (c) 2014 Steffen Trumtrar <s.trumtrar@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __SOC_SOCFPGA_L3NIC_H__
#define __SOC_SOCFPGA_L3NIC_H__

#ifdef CONFIG_ARCH_SOCFPGA

#define L3NIC_REMAP			0x0

/* Security Registers */

#define L3NIC_L4MAIN			0x8
#define L3NIC_L4SP			0xc
#define L3NIC_L4MP			0x10
#define L3NIC_L4OSC1			0x14
#define L3NIC_L4SPIM			0x18
#define L3NIC_STM			0x1c
#define L3NIC_LWHPS2FPGAREGS		0x20

#define L3NIC_USB1			0x28
#define L3NIC_NANDDATA			0x2c

#define L3NIC_USB0			0x80
#define L3NIC_NANDREGS			0x84
#define L3NIC_QSPIDATA			0x88
#define L3NIC_FPGAMGRDATA		0x8c
#define L3NIC_HPS2FPGAREGS		0x90
#define L3NIC_ACP			0x94
#define L3NIC_ROM			0x98
#define L3NIC_OCRAM			0x9c
#define L3NIC_SDRDATA			0xa0

/* ID registers */

#define L3NIC_PERIPH_ID_4		0x1fd0

#define L3NIC_PERIPH_ID_0		0x1fe0
#define L3NIC_PERIPH_ID_1		0x1fe4
#define L3NIC_PERIPH_ID_2		0x1fe8
#define L3NIC_PERIPH_ID_3		0x1fec
#define L3NIC_COMP_ID_0			0x1ff0
#define L3NIC_COMP_ID_1			0x1ff4
#define L3NIC_COMP_ID_2			0x1ff8
#define L3NIC_COMP_ID_3			0x1ffc

/* Master Registers */

#define L3NIC_L4_MAIN_FN_MOD_BM_ISS	0x2008

#define L3NIC_L4_SP_FN_MOD_BM_ISS	0x3008

#define L3NIC_L4_MP_FN_MOD_BM_ISS	0x4008

#define L3NIC_L4_OSC1_FN_MOD_BM_ISS	0x5008

#define L3NIC_L4_SPIM_FN_MOD_BM_ISS	0x6008

#define L3NIC_STM_FN_MOD_BM_ISS		0x7008

#define L3NIC_STM_FN_MOD		0x7108

#define L3NIC_LWHPS2FPGA_FN_MOD_BM_ISS	0x8008

#define L3NIC_LWHPS2FPGA_FN_MOD		0x8108

#define L3NIC_USB1_FN_MOD_BM_ISS	0xa008

#define L3NIC_USB1_AHB_CNTL		0xa044

#define L3NIC_NANDDATA_FN_MOD_BM_ISS	0xb008

#define L3NIC_NANDDATA_FN_MOD		0xb108

#define L3NIC_USB0_FN_MOD_BM_ISS	0x20008

#define L3NIC_USB0_AHB_CNTL		0x20044

#define L3NIC_QSPIDATA_FN_MOD_BM_ISS	0x22008

#define L3NIC_QSPIDATA_AHB_CNTL		0x22044

#define L3NIC_FPGAMGRDATA_FN_MOD_BM_ISS	0x23008

#define L3NIC_FPGAMGRDATA_WR_TIDEMARK	0x23040

#define L3NIC_FPGAMGRDATA_FN_MOD	0x23108

#define L3NIC_HPS2FPGA_FN_MOD_BM_ISS	0x24008

#define L3NIC_HPS2FPGA_WR_TIDEMARK	0x24040

#define L3NIC_HPS2FPGA_FN_MOD		0x24108

#define L3NIC_ACP_FN_MOD_BM_ISS		0x25008

#define L3NIC_ACP_FN_MOD		0x25108

#define L3NIC_BOOT_ROM_FN_MOD_BM_ISS	0x26008

#define L3NIC_BOOT_ROM_FN_MOD		0x26108

#define L3NIC_OCRAM_FN_MOD_BM_ISS	0x27008

#define L3NIC_OCRAM_WR_TIDEMARK		0x27040

#define L3NIC_OCRAM_FN_MOD		0x27108

/* Slave Registers */

#define L3NIC_DAP_FN_MOD2		0x42024
#define L3NIC_DAP_FN_MOD_AHB		0x42028

#define L3NIC_DAP_READ_QOS		0x42100
#define L3NIC_DAP_WRITE_QOS		0x42104
#define L3NIC_DAP_FN_MOD		0x42108

#define L3NIC_MPU_READ_QOS		0x43100
#define L3NIC_MPU_WRITE_QOS		0x43104
#define L3NIC_MPU_FN_MOD		0x43108

#define L3NIC_SDMMC_FN_MOD_AHB		0x44028

#define L3NIC_SDMMC_READ_QOS		0x44100
#define L3NIC_SDMMC_WRITE_QOS		0x44104
#define L3NIC_SDMMC_FN_MOD		0x44108

#define L3NIC_DMA_READ_QOS		0x45100
#define L3NIC_DMA_WRITE_QOS		0x45104
#define L3NIC_DMA_FN_MOD		0x45108

#define L3NIC_FPGA2HPS_WR_TIDEMARK	0x46040

#define L3NIC_FPGA2HPS_READ_QOS		0x46100
#define L3NIC_FPGA2HPS_WRITE_QOS	0x46104
#define L3NIC_FPGA2HPS_FN_MOD		0x46108

#define L3NIC_ETR_READ_QOS		0x47100
#define L3NIC_ETR_WRITE_QOS		0x47104
#define L3NIC_ETR_FN_MOD		0x47108

#define L3NIC_EMAC0_READ_QOS		0x48100
#define L3NIC_EMAC0_WRITE_QOS		0x48104
#define L3NIC_EMAC0_FN_MOD		0x48108

#define L3NIC_EMAC1_READ_QOS		0x49100
#define L3NIC_EMAC1_WRITE_QOS		0x49104
#define L3NIC_EMAC1_FN_MOD		0x49108

#define L3NIC_USB0_FN_MOD_AHB		0x4a028

#define L3NIC_USB0_READ_QOS		0x4a100
#define L3NIC_USB0_WRITE_QOS		0x4a104
#define L3NIC_USB0_FN_MOD		0x4a108

#define L3NIC_NAND_READ_QOS		0x4b100
#define L3NIC_NAND_WRITE_QOS		0x4b104
#define L3NIC_NAND_FN_MOD		0x4b108

#define L3NIC_USB1_FN_MOD_AHB		0x4c028

#define L3NIC_USB1_READ_QOS		0x4c100
#define L3NIC_USB1_WRITE_QOS		0x4c104
#define L3NIC_USB1_FN_MOD		0x4c108

#define L3NIC_LWHPS2FPGA_VISIBILITY	BIT(4)
#define L3NIC_HPS2FPGA_VISIBILITY	BIT(3)

struct socfpga_l3nic {
	void __iomem		*base;
	struct regmap		*regmap;
};

extern struct regmap *socfpga_l3nic_regmap_by_phandle(struct device_node *np,
						      const char *name);

#else

struct regmap *socfpga_l3nic_regmap_by_phandle(struct device_node *np,
					       const char *name)
{
	return ERR_PTR(-ENOSYS);
}

#endif
#endif
