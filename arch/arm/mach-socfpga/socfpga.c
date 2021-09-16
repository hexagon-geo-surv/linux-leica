// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (C) 2012-2015 Altera Corporation
 */

#include <linux/delay.h>
#include <linux/irqchip.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/reset/socfpga.h>

#include <asm/hardware/cache-l2x0.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/cacheflush.h>

#include "core.h"

/* Offset of imgcfg_ctrl_00 register within i_fpga_mgr_fpgamgrregs block */
#define SOCFPGA_A10_FPGAMGR_CTRL00 0x70

/* Value that has to be written to imgcfg_ctrl_00 register to deconfigure
 * the FPGA. Sets the nCONFIG signal to CSS.
 */
#define SOCFPGA_A10_FPGAMGR_CTRL00_RESET_FPGA 0x6

void __iomem *sys_manager_base_addr;
void __iomem *rst_manager_base_addr;
void __iomem *sdr_ctl_base_addr;
unsigned long socfpga_cpu1start_addr;
void __iomem *clkmgr_base_addr;
void __iomem *fpga_mgr_base_addr;
void __iomem *gpio1_base_addr;

static void __init socfpga_sysmgr_init(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "altr,sys-mgr");

	if (of_property_read_u32(np, "cpu1-start-addr",
			(u32 *) &socfpga_cpu1start_addr))
		pr_err("SMP: Need cpu1-start-addr in device tree.\n");

	/* Ensure that socfpga_cpu1start_addr is visible to other CPUs */
	smp_wmb();
	sync_cache_w(&socfpga_cpu1start_addr);

	sys_manager_base_addr = of_iomap(np, 0);

	np = of_find_compatible_node(NULL, NULL, "altr,rst-mgr");
	rst_manager_base_addr = of_iomap(np, 0);

	np = of_find_compatible_node(NULL, NULL, "altr,clk-mgr");
	clkmgr_base_addr = of_iomap(np, 0);
	WARN_ON(!clkmgr_base_addr);

	np = of_find_compatible_node(NULL, NULL, "altr,sdr-ctl");
	sdr_ctl_base_addr = of_iomap(np, 0);
}

static void __init socfpga_init_irq(void)
{
	irqchip_init();
	socfpga_sysmgr_init();
	if (IS_ENABLED(CONFIG_EDAC_ALTERA_L2C))
		socfpga_init_l2_ecc();

	if (IS_ENABLED(CONFIG_EDAC_ALTERA_OCRAM))
		socfpga_init_ocram_ecc();
	socfpga_reset_init();
}

static void __init socfpga_arria10_init_irq(void)
{
	irqchip_init();
	socfpga_sysmgr_init();
	if (IS_ENABLED(CONFIG_EDAC_ALTERA_L2C))
		socfpga_init_arria10_l2_ecc();
	if (IS_ENABLED(CONFIG_EDAC_ALTERA_OCRAM))
		socfpga_init_arria10_ocram_ecc();
	socfpga_reset_init();
}

/* KREA-specific initialization and power-off routines */
static void krea_power_off(void)
{
	/* We have to drive HPS_xRDY (GPIO19) signal high.
	 * In addition, switch peripheral power off by driving
	 * PER_PWR_xEN (GPIO14) high.
	 * Both GPIOs are attached to GPIO bank 1.
	 */
	u32 val, gpio19_mask = 1 << 19, gpio14_mask = 1 << 14;

	if (!gpio1_base_addr)
		return;

	/* Configure direction. */
	val = readl(gpio1_base_addr + SOCFPGA_A10_GPIO_DDR);
	writel(val | gpio19_mask | gpio14_mask,
			gpio1_base_addr + SOCFPGA_A10_GPIO_DDR);

	/* Set HPS_xRDY (GPIO19) value first */
	val = readl(gpio1_base_addr + SOCFPGA_A10_GPIO_DR);
	writel(val | gpio19_mask, gpio1_base_addr + SOCFPGA_A10_GPIO_DR);

	/* Inhibit a 100 msec delay to allow EFI on KBAT to ACK the HPS_xRDY */
	mdelay(100);

	/* Finally, set the PER_PWR_xEN (GPIO14) to switch all peripheral off */
	val = readl(gpio1_base_addr + SOCFPGA_A10_GPIO_DR);
	writel(val | gpio14_mask, gpio1_base_addr + SOCFPGA_A10_GPIO_DR);
}

static void __init krea_init_late(void)
{
	struct device_node *np;

	np = of_find_node_opts_by_path("krea_gpio", NULL);
	gpio1_base_addr = of_iomap(np, 0);
	WARN_ON(!gpio1_base_addr);

	np = of_find_compatible_node(NULL, NULL, "altr,socfpga-a10-fpga-mgr");
	fpga_mgr_base_addr = of_iomap(np, 0);
	WARN_ON(!fpga_mgr_base_addr);

	pm_power_off = &krea_power_off;
}

static void socfpga_cyclone5_restart(enum reboot_mode mode, const char *cmd)
{
	u32 temp;

	/* Turn on all periph PLL clocks */
	writel(0xffff, clkmgr_base_addr + SOCFPGA_ENABLE_PLL_REG);

	temp = readl(rst_manager_base_addr + SOCFPGA_RSTMGR_CTRL);

	if (mode == REBOOT_WARM)
		temp |= RSTMGR_CTRL_SWWARMRSTREQ;
	else
		temp |= RSTMGR_CTRL_SWCOLDRSTREQ;
	writel(temp, rst_manager_base_addr + SOCFPGA_RSTMGR_CTRL);
}

static void socfpga_arria10_restart(enum reboot_mode mode, const char *cmd)
{
	u32 temp;

	temp = readl(rst_manager_base_addr + SOCFPGA_A10_RSTMGR_CTRL);

	if (mode == REBOOT_WARM)
		temp |= RSTMGR_CTRL_SWWARMRSTREQ;
	else
		temp |= RSTMGR_CTRL_SWCOLDRSTREQ;

	/* Force FPGA reset. */
	writel(SOCFPGA_A10_FPGAMGR_CTRL00_RESET_FPGA,
			fpga_mgr_base_addr + SOCFPGA_A10_FPGAMGR_CTRL00);

	writel(temp, rst_manager_base_addr + SOCFPGA_A10_RSTMGR_CTRL);
}

static const char * const altera_dt_match[] = {
	"altr,socfpga",
	NULL
};

DT_MACHINE_START(SOCFPGA, "Altera SOCFPGA")
	.l2c_aux_val	= 0,
	.l2c_aux_mask	= ~0,
	.init_irq	= socfpga_init_irq,
	.restart	= socfpga_cyclone5_restart,
	.dt_compat	= altera_dt_match,
MACHINE_END

static const char * const altera_a10_dt_match[] = {
	"altr,socfpga-arria10",
	NULL
};

DT_MACHINE_START(SOCFPGA_A10, "Altera SOCFPGA Arria10")
	.l2c_aux_val	= 0,
	.l2c_aux_mask	= ~0,
	.init_irq	= socfpga_arria10_init_irq,
	.init_late	= krea_init_late,
	.restart	= socfpga_arria10_restart,
	.dt_compat	= altera_a10_dt_match,
MACHINE_END
