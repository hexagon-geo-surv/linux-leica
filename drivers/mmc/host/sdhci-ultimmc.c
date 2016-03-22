/*
 * sdhci-ultimmc.c Support for SDHCI UltiMMC cora on Altera FPGA.
 *
 * Author: Giovanni Pavoni Exor s.p.a.
 * Based on sdhci-cns3xxx.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>

#include "sdhci-pltfm.h"

/*
 * Private data structure to be populated with any eventual additional
 * field we need
 */
struct sdhci_ultimmc_priv {
	u32 dummy;
};

/*
 * Implementation of the I/O accessor functions.
 * 
 * NOTE: We need to use our custom accessor functions
 * (CONFIG_MMC_SDHCI_IO_ACCESSOR is defined) since the register
 * offsets are 4 bits shifted to the left
 */  

static u32 sdhci_ultimmc_readl(struct sdhci_host *host, int reg)
{
	reg = reg << 4;
	return readl(host->ioaddr + reg);
}

static u16 sdhci_ultimmc_readw(struct sdhci_host *host, int reg)
{
	reg = reg << 4;
	return readw(host->ioaddr + reg);
}

static u8 sdhci_ultimmc_readb(struct sdhci_host *host, int reg)
{
	reg = reg << 4;
	return readb(host->ioaddr + reg);
}

static void sdhci_ultimmc_writel(struct sdhci_host *host, u32 val, int reg)
{
	reg = reg << 4;
	writel(val, host->ioaddr + reg);
}

static void sdhci_ultimmc_writew(struct sdhci_host *host, u16 val, int reg)
{
	if (reg == SDHCI_CLOCK_CONTROL) {
		/* Force <50Mhz clock */
		if (((val >> SDHCI_DIVIDER_SHIFT) & SDHCI_DIV_MASK) < 0x01) {
			val |= (0x01 & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
		}
	}
	reg = reg << 4;
	writew(val, host->ioaddr + reg);
}

static void sdhci_ultimmc_writeb(struct sdhci_host *host, u8 val, int reg)
{
	reg = reg << 4;
	writeb(val, host->ioaddr + reg);
}

static struct sdhci_ops sdhci_ultimmc_ops = {
	.read_b	= sdhci_ultimmc_readb,
	.read_w	= sdhci_ultimmc_readw,
	.read_l	= sdhci_ultimmc_readl,
	.write_b= sdhci_ultimmc_writeb,
	.write_w= sdhci_ultimmc_writew,
	.write_l= sdhci_ultimmc_writel,
	.reset = sdhci_reset,
	.set_clock = sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.get_max_clock  = sdhci_pltfm_clk_get_max_clock,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
};

static struct sdhci_pltfm_data sdhci_ultimmc_pdata = {
	.ops	= &sdhci_ultimmc_ops,
	.quirks	= SDHCI_QUIRK_NO_SIMULT_VDD_AND_POWER |
		/*
		 * NOTE: This below flag is undefined to try to use the
		 * "busy" IRQ to avoid wasting CPU time
		 */
		/* SDHCI_QUIRK_NO_BUSY_IRQ |  */
		  SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DELAY_AFTER_POWER |
		/* SDHCI_QUIRK_NO_MULTIBLOCK | */
		  SDHCI_QUIRK_NO_HISPD_BIT | 
		  SDHCI_QUIRK_BROKEN_DMA |   /* Force disabling of DMA */
		  SDHCI_QUIRK_BROKEN_ADMA,
};

static int sdhci_ultimmc_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_ultimmc_priv *priv;
	int ret;

	dev_dbg(&pdev->dev, "probe\n");
	priv = devm_kzalloc(&pdev->dev, sizeof(struct sdhci_ultimmc_priv),
			    GFP_KERNEL);
	if (!priv) {
		dev_err(&pdev->dev, "unable to allocate private data");
		return -ENOMEM;
	}

	dev_dbg(&pdev->dev, "%s 1 \n", __func__);
	host = sdhci_pltfm_init(pdev, &sdhci_ultimmc_pdata, 0);
	if (IS_ERR(host)) {
		ret = PTR_ERR(host);
		goto err_sdhci_pltfm_init;
	}

	dev_dbg(&pdev->dev, "%s 2 \n", __func__);
	pltfm_host = sdhci_priv(host);
	pltfm_host->priv = priv;

	dev_dbg(&pdev->dev, "%s 3 \n", __func__);
	sdhci_get_of_property(pdev);

	dev_dbg(&pdev->dev, "%s 4 \n", __func__);

	ret = sdhci_add_host(host);
	if (ret)
		goto err_sdhci_add;

	dev_dbg(&pdev->dev, "%s OK\n", __func__);
	return 0;

err_sdhci_add:
	sdhci_pltfm_free(pdev);
err_sdhci_pltfm_init:
	return ret;
}

static int sdhci_ultimmc_remove(struct platform_device *pdev)
{
	/*struct sdhci_host *host = platform_get_drvdata(pdev);*/
	/*struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);*/
	/*struct sdhci_ultimmc_priv *priv = pltfm_host->priv;*/

	sdhci_pltfm_unregister(pdev);
	return 0;
}

static const struct of_device_id sdhci_ultimmc_of_match_table[] = {
	{ .compatible = "sdhci-ultimmc", },
	{}
};
MODULE_DEVICE_TABLE(of, sdhci_ultimmc_of_match_table);

static struct platform_driver sdhci_ultimmc_driver = {
	.driver		= {
		.name	= "sdhci-ultimmc",
		.owner	= THIS_MODULE,
		.pm	= SDHCI_PLTFM_PMOPS,
		.of_match_table = of_match_ptr(sdhci_ultimmc_of_match_table),
	},
	.probe		= sdhci_ultimmc_probe,
	.remove		= sdhci_ultimmc_remove,
};

module_platform_driver(sdhci_ultimmc_driver);

MODULE_DESCRIPTION("SDHCI driver for ultimmc");
MODULE_AUTHOR("Giovanni Pavoni");
MODULE_LICENSE("GPL v2");
