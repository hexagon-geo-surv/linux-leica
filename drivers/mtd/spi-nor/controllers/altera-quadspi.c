// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel FPGA Generic QUAD SPI Controller Core Driver
 *
 * Copyright (C) 2021 Andrey Zhizhikin <andrey.z@gmail.com>
 *
 * This driver covers only Version 1 of the Controller,
 * which can be instantiated in FPGA fabric. It is used to
 * read and program Configuration Devices, and is not
 * JEDEC-compliant.
 *
 * NOTE: Original Driver contained locking support, which is
 * not present in this version.
 *
 * Based on Altera QuadSPI Controller driver:
 * Copyright (C) 2014 Altera Corporation. All rights reserved
 *
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ALTERA_QUADSPI_RESOURCE_NAME	"altera_quadspi"

/* max possible slots for serial flash chip in the QUADSPI controller */
#define QUADSPI_MAX_CHIP_NUM	3

/* Status register */
#define QUADSPI_SR			0x0
#define QUADSPI_SR_MASK		GENMASK(3, 0)
#define  QUADSPI_SR_WIP		BIT(0)
#define  QUADSPI_SR_WEL		BIT(1)
#define  QUADSPI_SR_BP0		BIT(2)
#define  QUADSPI_SR_BP1		BIT(3)
#define  QUADSPI_SR_BP2		BIT(4)
#define  QUADSPI_SR_BP3		BIT(5)
#define  QUADSPI_SR_TB		BIT(6)

/* Device id register */
#define QUADSPI_SID			0x4
#define QUADSPI_RDID		0x8
#define QUADSPI_ID_MASK		GENMASK(7, 0)

/*
 * Memory operation register
 *
 * This register is used to do memory protect and erase operations
 *
 */
#define QUADSPI_MEM_OP						0xC

#define QUADSPI_MEM_OP_CMD_MASK					GENMASK(1, 0)
#define QUADSPI_MEM_OP_BULK_ERASE_CMD			0x1
#define QUADSPI_MEM_OP_SECTOR_ERASE_CMD			0x2
#define QUADSPI_MEM_OP_SECT_VALUE_MASK			GENMASK(17, 8)
#define QUADSPI_MEM_OP_SECT_PROT_CMD			0x3
#define QUADSPI_MEM_OP_SECT_PROT_VALUE_MASK		GENMASK(12 8)

/* sector value should occupy bits 17:8 */
/* sector erase commands occupies lower 2 bits */
#define QUADSPI_MEM_SECT_ERASE(sector) \
	(((sector << 8) & QUADSPI_MEM_OP_SECT_VALUE_MASK) | \
	QUADSPI_MEM_OP_SECTOR_ERASE_CMD)

#define QUADSPI_MEM_SECT_PROT(sr_tb, sr_bp) \
	((((sr_tb << 12) | (sr_bp << 8)) & \
	  QUADSPI_MEM_OP_SECT_PROT_VALUE_MASK) |                               \
	 QUADSPI_MEM_OP_SECT_PROT_CMD)

/*
 * Interrupt Status register
 *
 * This register is used to determine whether an invalid write or
 * erase operation triggered an interrupt. Bits are set by controller,
 * indicating:
 * - Illegal Erase: bit 0
 * - Illegal Write: bit 1
 *
 */
#define QUADSPI_ISR					0x10
#define QUADSPI_ISR_FLAG(wr_or_er_verify) \
	(!!(wr_or_er_verify) ? BIT(1) : BIT(0))

/*
 * Interrupt Mask register
 *
 * This register is used to mask the invalid erase or the invalid
 * write interrupts.
 *
 */
#define QUADSPI_IMR					0x14
#define QUADSPI_IMR_ILLEGAL_ERASE	BIT(0)
#define QUADSPI_IMR_ILLEGAL_WRITE	BIT(1)

/*
 * Chip Select register
 *
 * This register is used to set the CS to selects which chip
 * receives commands and IO requests.
 *
 */
#define QUADSPI_CS			0x18
#define QUADSPI_CS_NUM(cs)	((cs) & GENMASK(2, 0))

struct altera_quadspi {
	struct device *dev;
	struct mutex lock;

	u32 opcode_id;
	void __iomem *csr_base;
	void __iomem *data_base;
	u32 num_flashes;
	struct spi_nor *nor[QUADSPI_MAX_CHIP_NUM];
};

struct altera_quadspi_priv {
	struct altera_quadspi *controller;
	u32 bank;
	u8 device_id;
};

static int altera_quadspi_write_reg(struct spi_nor *nor, u8 opcode, const u8 *buf,
			       size_t len)
{
	return 0;
}

static int altera_quadspi_read_reg(struct spi_nor *nor, u8 opcode, u8 *val,
				   size_t len)
{
	struct altera_quadspi_priv *priv = nor->priv;
	struct altera_quadspi *controller = priv->controller;
	u32 data = 0;

	memset(val, 0, len);

	/* Since this Controller does not conform to JEDEC specification,
	 * only limited number or regsters are allowed here. All other
	 * read operations would return 0 as a result of read operation.
	 */
	switch (opcode) {
	case SPINOR_OP_RDSR:
		data = readl(controller->csr_base + QUADSPI_SR);
		*val = (u8)data & QUADSPI_SR_MASK;
		break;
	case SPINOR_OP_RDID:
		data = readl(controller->csr_base + QUADSPI_RDID);
		*val = (u8)data & QUADSPI_ID_MASK; /* device id */
		break;
	default:
		*val = 0;
		break;
	}

	return 0;
}

static int altera_quadspi_write_erase_check(struct spi_nor *nor,
					    bool write_erase)
{
	struct altera_quadspi_priv *flash = nor->priv;
	struct altera_quadspi *controller = flash->controller;
	u32 val;

	val = readl(controller->csr_base + QUADSPI_ISR);
	if (val & QUADSPI_ISR_FLAG(write_erase)) {
		dev_err(nor->dev,
			"write/erase failed, sector might be protected\n");
		/* clear this status for next use */
		writel(val, controller->csr_base + QUADSPI_ISR);
		return -EIO;
	}
	return 0;
}

static int altera_quadspi_erase(struct spi_nor *nor, loff_t offset)
{
	struct altera_quadspi_priv *flash = nor->priv;
	struct altera_quadspi *controller = flash->controller;
	int sector;

	sector = mtd_div_by_eb(offset, &nor->mtd);
	/* sanity check that block_offset is a valid sector number */
	if (sector < 0)
		return -EINVAL;

	/* write sector erase command to QUADSPI_MEM_OP register */
	writel(QUADSPI_MEM_SECT_ERASE(sector),
	       controller->csr_base + QUADSPI_MEM_OP);

	return altera_quadspi_write_erase_check(nor, false);
}

static int altera_quadspi_prep(struct spi_nor *nor)
{
	struct altera_quadspi_priv *flash = nor->priv;

	mutex_lock(&flash->controller->lock);
	return 0;
}

static void altera_quadspi_unprep(struct spi_nor *nor)
{
	struct altera_quadspi_priv *flash = nor->priv;

	mutex_unlock(&flash->controller->lock);
}

static int altera_quadspi_read(struct spi_nor *nor, loff_t from, size_t len,
			      u_char *buf)
{
	struct altera_quadspi_priv *flash = nor->priv;
	struct altera_quadspi *controller = flash->controller;

	memcpy_fromio(buf, controller->data_base + from, len);

	return len;
}

static int altera_quadspi_write(struct spi_nor *nor, loff_t to, size_t len,
			       const u_char *buf)
{
	struct altera_quadspi_priv *flash = nor->priv;
	struct altera_quadspi *controller = flash->controller;

	memcpy_toio(controller->data_base + to, buf, len);

	/* check whether write triggered a illegal write interrupt */
	altera_quadspi_write_erase_check(nor, true);

	return len;
}

static int altera_quadspi_id_read(struct spi_nor *nor)
{
	struct altera_quadspi_priv *priv = nor->priv;

	int ret = nor->controller_ops->read_reg(nor, SPINOR_OP_RDID,
			&priv->device_id, 1);
	return ret;
}

static const struct spi_nor_controller_ops quadspi_controller_ops = {
	.prepare = altera_quadspi_prep,
	.unprepare = altera_quadspi_unprep,
	.read_reg = altera_quadspi_read_reg,
	.write_reg = altera_quadspi_write_reg,
	.read = altera_quadspi_read,
	.write = altera_quadspi_write,
	.erase = altera_quadspi_erase,
};

static int altera_quadspi_setup_flash(struct device_node *np,
				struct altera_quadspi *host)
{
	const struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
			SNOR_HWCAPS_READ_FAST |
			SNOR_HWCAPS_READ_1_1_4 |
			SNOR_HWCAPS_PP,
	};
	struct device *dev = host->dev;
	struct spi_nor *nor;
	struct altera_quadspi_priv *priv;
	struct mtd_info *mtd;
	int ret = 0;

	nor = devm_kzalloc(dev, sizeof(*nor), GFP_KERNEL);
	if (!nor)
		return -ENOMEM;

	nor->dev = dev;
	spi_nor_set_flash_node(nor, np);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = of_property_read_u32(np, "reg", &priv->bank);
	if (ret) {
		dev_err(dev, "There's no reg property for %pOF\n",
			np);
		return ret;
	}

	priv->controller = host;
	nor->priv = priv;
	nor->controller_ops = &quadspi_controller_ops;

	if (priv->bank < 3)
		writel(QUADSPI_CS_NUM(priv->bank), host->csr_base + QUADSPI_CS);
	else {
		dev_err(dev, "bank %d is out of range\n", priv->bank);
		return -ENODEV;
	}

	ret = altera_quadspi_id_read(nor);
	if (ret)
		return ret;

	/* Issue scan and provide the name from binding, since we know
	 * which flash should be connected, and there is no need to use
	 * JEDEC scan. This is done because Controller and Configuration
	 * devices connected are not JEDEC-compliant.
	 */
	ret = spi_nor_scan(nor, np->name, &hwcaps);
	if (ret) {
		dev_err(dev, "spi_nor_scan failed: %d\n", ret);
		return ret;
	}

	mtd = &nor->mtd;
	mtd->name = np->name;
	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "mtd_device_register failed: %d\n", ret);
		return ret;
	}

	host->nor[host->num_flashes] = nor;
	host->num_flashes++;

	return 0;
}

static void altera_quadspi_unregister_all(struct altera_quadspi *host)
{
	int i;

	for (i = 0; i < host->num_flashes; i++)
		mtd_device_unregister(&host->nor[i]->mtd);
}

static int altera_quadspi_register_all(struct altera_quadspi *host)
{
	struct device *dev = host->dev;
	struct device_node *np;
	int ret;

	for_each_available_child_of_node(dev->of_node, np) {
		ret = altera_quadspi_setup_flash(np, host);
		if (ret) {
			dev_err(dev, "flash chip %s failed to register\n", np->name);
			of_node_put(np);
			goto fail;
		}

		if (host->num_flashes == QUADSPI_MAX_CHIP_NUM) {
			dev_warn(dev, "Flash device number exceeds the maximum chipselect number\n");
			of_node_put(np);
			break;
		}
	}

	return 0;
fail:
	dev_err(dev, "failed to register chip\n");
	altera_quadspi_unregister_all(host);
	return ret;
}

static int altera_quadspi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct altera_quadspi *quadspi;
	int ret = 0;

	quadspi = devm_kzalloc(dev, sizeof(*quadspi), GFP_KERNEL);
	if (!quadspi)
		return -ENOMEM;

	platform_set_drvdata(pdev, quadspi);
	quadspi->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "avl_csr");
	quadspi->csr_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(quadspi->csr_base)) {
		dev_err(&pdev->dev, "failed to map csr base\n");
		return PTR_ERR(quadspi->csr_base);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "avl_mem");
	quadspi->data_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(quadspi->data_base)) {
		dev_err(&pdev->dev, "failed to map data base\n");
		return PTR_ERR(quadspi->data_base);
	}

	mutex_init(&quadspi->lock);

	ret = altera_quadspi_register_all(quadspi);
	if (ret) {
		dev_err(&pdev->dev, "failed to register flash chips\n");
		mutex_destroy(&quadspi->lock);
	}

	return ret;
}

static int altera_quadspi_remove(struct platform_device *pdev)
{
	struct altera_quadspi *host = platform_get_drvdata(pdev);

	altera_quadspi_unregister_all(host);
	mutex_destroy(&host->lock);

	return 0;
}

static const struct of_device_id altera_quadspi_id_table[] = {
	{ .compatible = "altr,quadspi-1.0" },
	{}
};
MODULE_DEVICE_TABLE(of, altera_quadspi_id_table);

static struct platform_driver altera_quadspi_driver = {
	.driver = {
		.name = ALTERA_QUADSPI_RESOURCE_NAME,
		.of_match_table = altera_quadspi_id_table,
	},
	.probe = altera_quadspi_probe,
	.remove = altera_quadspi_remove,
};
module_platform_driver(altera_quadspi_driver);

MODULE_AUTHOR("Viet Nga Dao <vndao@altera.com>");
MODULE_AUTHOR("Andrey Zhizhikin <andrey.z@gmail.com>");
MODULE_DESCRIPTION("Altera QuadSPI Driver");
MODULE_LICENSE("GPL v2");
