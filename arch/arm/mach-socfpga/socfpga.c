/*
 *  Copyright (C) 2012-2015 Altera Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/delay.h>
#include <linux/perf/arm_pmu.h>
#include <linux/irqchip.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>

#include <asm/hardware/cache-l2x0.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/cacheflush.h>

#include "core.h"
#include "socfpga_cti.h"
#include "l2_cache.h"
#include "ocram.h"
#include "nand.h"

/* Offset of imgcfg_ctrl_00 register within i_fpga_mgr_fpgamgrregs block */
#define SOCFPGA_A10_FPGAMGR_CTRL00 0x70
/* Value that has to be written to imgcfg_ctrl_00 register to deconfigure
 * the FPGA. Sets the nCONFIG signal to CSS.
 */
#define SOCFPGA_A10_FPGAMGR_CTRL00_RESET_FPGA 0x6

void __iomem *sys_manager_base_addr;
void __iomem *rst_manager_base_addr;
void __iomem *sdr_ctl_base_addr;
void __iomem *fpga_mgr_base_addr;
unsigned long socfpga_cpu1start_addr;
void __iomem *clkmgr_base_addr;
void __iomem *gpio1_base_addr;

static int socfpga_is_a10(void);

#ifdef CONFIG_HW_PERF_EVENTS
static struct arm_pmu_platdata socfpga_pmu_platdata = {
	.handle_irq = socfpga_pmu_handler,
	.init = socfpga_init_cti,
	.start = socfpga_start_cti,
	.stop = socfpga_stop_cti,
};
#endif

static const struct of_dev_auxdata socfpga_auxdata_lookup[] __initconst = {
#ifdef CONFIG_HW_PERF_EVENTS
	OF_DEV_AUXDATA("arm,cortex-a9-pmu", 0, "arm-pmu", &socfpga_pmu_platdata),
#endif
	{ /* sentinel */ }
};

static void __init socfpga_soc_device_init(void)
{
	struct device_node *root;
	struct soc_device *soc_dev;
	struct soc_device_attribute *soc_dev_attr;
	const char *machine;
	u32 id = SOCFPGA_ID_DEFAULT;
	u32 rev = SOCFPGA_REVISION_DEFAULT;
	int err;

	root = of_find_node_by_path("/");
	if (!root)
		return;

	err = of_property_read_string(root, "model", &machine);
	if (err)
		return;

	of_node_put(root);

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return;

	/* Read Silicon ID from System manager */
	if (sys_manager_base_addr) {
		id =  __raw_readl(sys_manager_base_addr +
			SYSMGR_SILICON_ID1_OFFSET);
		rev = (id & SYSMGR_SILICON_ID1_REV_MASK)
				>> SYSMGR_SILICON_ID1_REV_SHIFT;
		id = (id & SYSMGR_SILICON_ID1_ID_MASK)
				>> SYSMGR_SILICON_ID1_ID_SHIFT;
	}

	soc_dev_attr->soc_id = kasprintf(GFP_KERNEL, "%u", id);
	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%d", rev);
	soc_dev_attr->machine = kasprintf(GFP_KERNEL, "%s", machine);
	soc_dev_attr->family = "SOCFPGA";

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR_OR_NULL(soc_dev)) {
		kfree(soc_dev_attr->soc_id);
		kfree(soc_dev_attr->machine);
		kfree(soc_dev_attr->revision);
		kfree(soc_dev_attr);
		return;
	}

	return;
}

static void __init enable_periphs(void)
{
	if (socfpga_is_a10()) {
		/* temp hack to enable all periphs from reset for A10 */
		writel(0x0, rst_manager_base_addr + SOCFPGA_A10_RSTMGR_PER0MODRST);
		writel(0x0, rst_manager_base_addr + SOCFPGA_A10_RSTMGR_PER1MODRST);
	} else {
		writel(0x0, rst_manager_base_addr + SOCFPGA_RSTMGR_MODPERRST);
	}
}

static int socfpga_is_a10(void)
{
	return of_machine_is_compatible("altr,socfpga-arria10");
}

void __init socfpga_sysmgr_init(void)
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

	if (socfpga_is_a10() > 0) {
		np = of_find_compatible_node(NULL, NULL, "altr,socfpga-a10-fpga-mgr");
		fpga_mgr_base_addr = of_iomap(np, 0); 
		WARN_ON(!fpga_mgr_base_addr);
	}   
}

static void __init socfpga_init_irq(void)
{
	irqchip_init();
	socfpga_sysmgr_init();
	socfpga_init_l2_ecc();
	socfpga_init_arria10_l2_ecc();
}

static void socfpga_cyclone5_restart(enum reboot_mode mode, const char *cmd)
{
	u32 temp;

	/* Turn on all periph PLL clocks */
	writel(0xffff, clkmgr_base_addr + SOCFPGA_ENABLE_PLL_REG);

	temp = readl(rst_manager_base_addr + SOCFPGA_RSTMGR_CTRL);

	if (mode == REBOOT_HARD)
		temp |= RSTMGR_CTRL_SWCOLDRSTREQ;
	else
		temp |= RSTMGR_CTRL_SWWARMRSTREQ;
	writel(temp, rst_manager_base_addr + SOCFPGA_RSTMGR_CTRL);
}

static void socfpga_arria10_restart(enum reboot_mode mode, const char *cmd)
{
	u32 temp;

	temp = readl(rst_manager_base_addr + SOCFPGA_A10_RSTMGR_CTRL);

	if (mode == REBOOT_HARD)
		temp |= RSTMGR_CTRL_SWCOLDRSTREQ;
	else
		temp |= RSTMGR_CTRL_SWWARMRSTREQ;

	/* Force FPGA reset. */
	writel(SOCFPGA_A10_FPGAMGR_CTRL00_RESET_FPGA,
			fpga_mgr_base_addr + SOCFPGA_A10_FPGAMGR_CTRL00);

	writel(temp, rst_manager_base_addr + SOCFPGA_A10_RSTMGR_CTRL);
}

static void __init socfpga_cyclone5_init(void)
{
	of_platform_populate(NULL, of_default_bus_match_table,
				socfpga_auxdata_lookup, NULL);
	enable_periphs();
	socfpga_soc_device_init();
	socfpga_init_ocram_ecc();
	socfpga_init_nand_ecc();
}

void krea_power_off(void)
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

static void __init socfpga_arria10_init(void)
{
	struct device_node *np;

	of_platform_populate(NULL, of_default_bus_match_table,
				NULL, NULL);

	np = of_find_node_opts_by_path("krea_gpio", NULL);
	gpio1_base_addr = of_iomap(np, 0);

	if (!gpio1_base_addr)
		pr_warning("GPIO missing, won't disable HPS_xRDY\n");

	pm_power_off = &krea_power_off;
}

static const char *altera_dt_match[] = {
	"altr,socfpga",
	NULL
};

DT_MACHINE_START(SOCFPGA, "Altera SOCFPGA")
	.l2c_aux_val	= L310_AUX_CTRL_DATA_PREFETCH |
			L310_AUX_CTRL_INSTR_PREFETCH |
			L2C_AUX_CTRL_SHARED_OVERRIDE,
	.l2c_aux_mask	= ~0,
	.init_irq	= socfpga_init_irq,
	.init_machine	= socfpga_cyclone5_init,
	.restart	= socfpga_cyclone5_restart,
	.dt_compat	= altera_dt_match,
MACHINE_END

static const char *altera_a10_dt_match[] = {
	"altr,socfpga-arria10",
	NULL
};

DT_MACHINE_START(SOCFPGA_A10, "Altera SOCFPGA Arria10")
	.l2c_aux_val	= 0,
	.l2c_aux_mask	= ~0,
	.init_irq	= socfpga_init_irq,
	.init_machine	= socfpga_arria10_init,
	.restart	= socfpga_arria10_restart,
	.dt_compat	= altera_a10_dt_match,
MACHINE_END
