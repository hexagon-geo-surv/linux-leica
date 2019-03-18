/*
 * Copyright (C) 2014 Altera Corporation. All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/spi-nor.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sched.h>

#define ALTERA_QUADSPI_RESOURCE_NAME           "altera_quadspi"

/* max possible slots for serial flash chip in the QUADSPI controller */
#define MAX_NUM_FLASH_CHIP             3

#define EPCS_OPCODE_ID                 1
#define NON_EPCS_OPCODE_ID             2

#define WRITE_CHECK                    1
#define ERASE_CHECK                    0

/* Define max times to check status register before we give up. */
#define QUADSPI_MAX_TIME_OUT               (40 * HZ)

/* defines for status register */
#define QUADSPI_SR_REG                 0x0
#define QUADSPI_SR_WIP_MASK                0x00000001
#define QUADSPI_SR_WIP                 0x1
#define QUADSPI_SR_WEL                 0x2
#define QUADSPI_SR_BP0                 0x4
#define QUADSPI_SR_BP1                 0x8
#define QUADSPI_SR_BP2                 0x10
#define QUADSPI_SR_BP3                 0x40
#define QUADSPI_SR_TB                  0x20
#define QUADSPI_SR_MASK                    0x0000000F

/* defines for device id register */
#define QUADSPI_SID_REG                    0x4
#define QUADSPI_RDID_REG               0x8
#define QUADSPI_ID_MASK                    0x000000FF

/*
 * QUADSPI_MEM_OP register offset
 *
 * The QUADSPI_MEM_OP register is used to do memory protect and erase operations
 *
 */
#define QUADSPI_MEM_OP_REG             0xC

#define QUADSPI_MEM_OP_CMD_MASK                0x00000003
#define QUADSPI_MEM_OP_BULK_ERASE_CMD          0x00000001
#define QUADSPI_MEM_OP_SECTOR_ERASE_CMD            0x00000002
#define QUADSPI_MEM_OP_SECTOR_PROTECT_CMD      0x00000003
#define QUADSPI_MEM_OP_SECTOR_VALUE_MASK       0x0003FF00
#define QUADSPI_MEM_OP_SECTOR_PROTECT_VALUE_MASK   0x00001F00
#define QUADSPI_MEM_OP_SECTOR_PROTECT_SHIFT        8
/*
 * QUADSPI_ISR register offset
 *
 * The QUADSPI_ISR register is used to determine whether an invalid write or
 * erase operation trigerred an interrupt
 *
 */
#define QUADSPI_ISR_REG                    0x10

#define QUADSPI_ISR_ILLEGAL_ERASE_MASK         0x00000001
#define QUADSPI_ISR_ILLEGAL_WRITE_MASK         0x00000002

/*
 * QUADSPI_IMR register offset
 *
 * The QUADSPI_IMR register is used to mask the invalid erase or the invalid
 * write interrupts.
 *
 */
#define QUADSPI_IMR_REG                    0x14
#define QUADSPI_IMR_ILLEGAL_ERASE_MASK         0x00000001

#define QUADSPI_IMR_ILLEGAL_WRITE_MASK         0x00000002

#define QUADSPI_CHIP_SELECT_REG                0x18
#define QUADSPI_CHIP_SELECT_MASK           0x00000007
#define QUADSPI_CHIP_SELECT_0              0x00000001
#define QUADSPI_CHIP_SELECT_1              0x00000002
#define QUADSPI_CHIP_SELECT_2              0x00000004

struct altera_quadspi {
   u32 opcode_id;
   void __iomem *csr_base;
   void __iomem *data_base;
   u32 num_flashes;
   struct device *dev;
   struct altera_quadspi_flash *flash[MAX_NUM_FLASH_CHIP];
   struct device_node *np[MAX_NUM_FLASH_CHIP];
};

struct altera_quadspi_flash {
   struct spi_nor nor;
   struct altera_quadspi *q;
};

struct flash_device {
   char *name;
   u32 opcode_id;
   u32 device_id;
};

#define FLASH_ID(_n, _opcode_id, _id)  \
{                  \
   .name = (_n),           \
   .opcode_id = (_opcode_id),  \
   .device_id = (_id),     \
}

static struct flash_device flash_devices[] = {
   FLASH_ID("epcs16",              EPCS_OPCODE_ID,     0x14),
   FLASH_ID("epcs64",              EPCS_OPCODE_ID,     0x16),
   FLASH_ID("epcs128",             EPCS_OPCODE_ID,     0x18),

   FLASH_ID("epcq16",              NON_EPCS_OPCODE_ID, 0x15),
   FLASH_ID("epcq32",              NON_EPCS_OPCODE_ID, 0x16),
   FLASH_ID("epcq64",              NON_EPCS_OPCODE_ID, 0x17),
   FLASH_ID("epcq128",             NON_EPCS_OPCODE_ID, 0x18),
   FLASH_ID("epcq256",             NON_EPCS_OPCODE_ID, 0x19),
   FLASH_ID("epcq512",             NON_EPCS_OPCODE_ID, 0x20),
   FLASH_ID("epcq1024",            NON_EPCS_OPCODE_ID, 0x21),

   FLASH_ID("epcql256",            NON_EPCS_OPCODE_ID, 0x19),
   FLASH_ID("epcql512",            NON_EPCS_OPCODE_ID, 0x20),
   FLASH_ID("epcql1024",           NON_EPCS_OPCODE_ID, 0x21),

};

static int altera_quadspi_write_reg(struct spi_nor *nor, u8 opcode, u8 *val,
                   int len)
{
   return 0;
}

static int altera_quadspi_read_reg(struct spi_nor *nor, u8 opcode, u8 *val,
                  int len)
{
   struct altera_quadspi_flash *flash = nor->priv;
   struct altera_quadspi *q = flash->q;
   u32 data = 0;

   memset(val, 0, len);

   switch (opcode) {
   case SPINOR_OP_RDSR:
       data = readl(q->csr_base + QUADSPI_SR_REG);
       *val = (u8)data & QUADSPI_SR_MASK;
       break;
   case SPINOR_OP_RDID:
       if (q->opcode_id == EPCS_OPCODE_ID)
           data = readl(q->csr_base + QUADSPI_SID_REG);
       else
           data = readl(q->csr_base + QUADSPI_RDID_REG);

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
   struct altera_quadspi_flash *flash = nor->priv;
   struct altera_quadspi *q = flash->q;
   u32 val;
   u32 mask;

   if (write_erase)
       mask = QUADSPI_ISR_ILLEGAL_WRITE_MASK;
   else
       mask = QUADSPI_ISR_ILLEGAL_ERASE_MASK;

   val = readl(q->csr_base + QUADSPI_ISR_REG);
   if (val & mask) {
       dev_err(nor->dev,
           "write/erase failed, sector might be protected\n");
       /* clear this status for next use */
       writel(val, q->csr_base + QUADSPI_ISR_REG);
       return -EIO;
   }
   return 0;
}

static int altera_quadspi_addr_to_sector(struct mtd_info *mtd, uint64_t offset)
{
   if (mtd->erasesize_shift)
       return offset >> mtd->erasesize_shift;
   do_div(offset, mtd->erasesize);
   return offset;
}

static int altera_quadspi_erase(struct spi_nor *nor, loff_t offset)
{
   struct altera_quadspi_flash *flash = nor->priv;
   struct altera_quadspi *q = flash->q;
   struct mtd_info *mtd = &flash->nor.mtd;
   u32 val;
   int sector_value;

   sector_value = altera_quadspi_addr_to_sector(mtd, offset);
   /* sanity check that block_offset is a valid sector number */
   if (sector_value < 0)
       return -EINVAL;

   /* sector value should occupy bits 17:8 */
   val = (sector_value << 8) & QUADSPI_MEM_OP_SECTOR_VALUE_MASK;

   /* sector erase commands occupies lower 2 bits */
   val |= QUADSPI_MEM_OP_SECTOR_ERASE_CMD;

   /* write sector erase command to QUADSPI_MEM_OP register */
   writel(val, q->csr_base + QUADSPI_MEM_OP_REG);

   return altera_quadspi_write_erase_check(nor, ERASE_CHECK);
}

static int altera_quadspi_read(struct spi_nor *nor, loff_t from, size_t len,
                  size_t *retlen, u_char *buf)
{
   struct altera_quadspi_flash *flash = nor->priv;
   struct altera_quadspi *q = flash->q;

   memcpy_fromio(buf, q->data_base + from, len);
   *retlen = len;

   return 0;
}

static void altera_quadspi_write(struct spi_nor *nor, loff_t to, size_t len,
                size_t *retlen, const u_char *buf)
{
   struct altera_quadspi_flash *flash = nor->priv;
   struct altera_quadspi *q = flash->q;

   memcpy_toio(q->data_base + to, buf, len);
   *retlen += len;

   /* check whether write triggered a illegal write interrupt */
   altera_quadspi_write_erase_check(nor, WRITE_CHECK);
}

static int altera_quadspi_lock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
   struct altera_quadspi_flash *flash = nor->priv;
   struct altera_quadspi *q = flash->q;
   struct mtd_info *mtd = &nor->mtd;
   uint32_t offset = ofs;
   u32 sector_start, sector_end;
   uint64_t num_sectors;
   u32 mem_op;
   u32 sr_bp;
   u32 sr_tb;

   sector_start = offset;
   sector_end = altera_quadspi_addr_to_sector(mtd, offset + len);
   num_sectors = mtd->size;
   do_div(num_sectors, mtd->erasesize);

   dev_dbg(nor->dev, "%s: sector start is %u,sector end is %u\n",
       __func__, sector_start, sector_end);

   if (sector_start >= num_sectors / 2) {
       sr_bp = fls(num_sectors - 1 - sector_start) + 1;
       sr_tb = 0;
   } else if ((sector_end < num_sectors / 2) &&
         (q->opcode_id != EPCS_OPCODE_ID)) {
       sr_bp = fls(sector_end) + 1;
       sr_tb = 1;
   } else {
       sr_bp = 16;
       sr_tb = 0;
   }

   mem_op = (sr_tb << 12) | (sr_bp << 8);
   mem_op &= QUADSPI_MEM_OP_SECTOR_PROTECT_VALUE_MASK;
   mem_op |= QUADSPI_MEM_OP_SECTOR_PROTECT_CMD;
   writel(mem_op, q->csr_base + QUADSPI_MEM_OP_REG);

   return 0;
}

static int altera_quadspi_unlock(struct spi_nor *nor, loff_t ofs, uint64_t len)
{
   struct altera_quadspi_flash *flash = nor->priv;
   struct altera_quadspi *q = flash->q;
   u32 mem_op;

   dev_dbg(nor->dev, "Unlock all protected area\n");
   mem_op = QUADSPI_MEM_OP_SECTOR_PROTECT_CMD;
   writel(mem_op, q->csr_base + QUADSPI_MEM_OP_REG);

   return 0;
}

static void altera_quadspi_chip_select(struct altera_quadspi *q, u32 bank)
{
   u32 val = 0;

   switch (bank) {
   case 0:
       val = QUADSPI_CHIP_SELECT_0;
       break;
   case 1:
       val = QUADSPI_CHIP_SELECT_1;
       break;
   case 2:
       val = QUADSPI_CHIP_SELECT_2;
       break;
   default:
       dev_err(q->dev, "invalid bank\n");
       return;
   }
   writel(val, q->csr_base + QUADSPI_CHIP_SELECT_REG);
}

static int altera_quadspi_probe_config_dt(struct platform_device *pdev,
                     struct device_node *np,
                     struct altera_quadspi *q)
{
   struct device_node *pp;
   struct resource *quadspi_res;
   int i = 0;

   quadspi_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
                          "avl_csr");
   q->csr_base = devm_ioremap_resource(&pdev->dev, quadspi_res);
   if (IS_ERR(q->csr_base)) {
       dev_err(&pdev->dev, "%s: ERROR: failed to map csr base\n",
           __func__);
       return PTR_ERR(q->csr_base);
   }

   quadspi_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
                          "avl_mem");
   q->data_base = devm_ioremap_resource(&pdev->dev, quadspi_res);
   if (IS_ERR(q->data_base)) {
       dev_err(&pdev->dev, "%s: ERROR: failed to map data base\n",
           __func__);
       return PTR_ERR(q->data_base);
   }

   /* Fill structs for each subnode (flash device) */
   for_each_available_child_of_node(np, pp) {
       /* Read bank id from DT */
       q->np[i] = pp;
       i++;
   }
   q->num_flashes = i;
   return 0;
}

static int altera_quadspi_scan(struct spi_nor *nor, const char *name)
{
   struct altera_quadspi_flash *flash = nor->priv;
   struct altera_quadspi *q = flash->q;
   int index;
   int ret;
   u8 id = 0;

   ret = nor->read_reg(nor, SPINOR_OP_RDID, &id, 1);
   if (ret)
       return ret;

   for (index = 0; index < ARRAY_SIZE(flash_devices); index++) {
       if (flash_devices[index].device_id == id &&
           strcmp(name, flash_devices[index].name) == 0) {
           q->opcode_id = flash_devices[index].opcode_id;
           return 0;
       }
   }

   /* Memory chip is not listed and not supported */
   return -EINVAL;
}

static int altera_quadspi_setup_banks(struct platform_device *pdev,
                     u32 bank, struct device_node *np)
{
   struct altera_quadspi *q = platform_get_drvdata(pdev);
   struct mtd_part_parser_data ppdata = {};
   struct altera_quadspi_flash *flash;
   struct spi_nor *nor;
   int ret = 0;
   char modalias[40];

   if (bank > q->num_flashes - 1)
       return -EINVAL;

   altera_quadspi_chip_select(q, bank);

   flash = devm_kzalloc(q->dev, sizeof(*flash), GFP_KERNEL);
   if (!flash)
       return -ENOMEM;

   q->flash[bank] = flash;
   nor = &flash->nor;
   nor->dev = &pdev->dev;
   nor->priv = flash;
   nor->mtd.priv = nor;
   flash->q = q;

   /* spi nor framework*/
   nor->read_reg = altera_quadspi_read_reg;
   nor->write_reg = altera_quadspi_write_reg;
   nor->read = altera_quadspi_read;
   nor->write = altera_quadspi_write;
   nor->erase = altera_quadspi_erase;
   nor->flash_lock = altera_quadspi_lock;
   nor->flash_unlock = altera_quadspi_unlock;

   /* scanning flash and checking dev id */
   if (of_modalias_node(np, modalias, sizeof(modalias)) < 0)
       return -EINVAL;

   ret = altera_quadspi_scan(nor, modalias);
   if (ret) {
       dev_err(nor->dev, "flash not found\n");
       return ret;
   }

   ret = spi_nor_scan(nor, modalias, SPI_NOR_QUAD);
   if (ret)
       return ret;

   ppdata.of_node = np;

   return mtd_device_parse_register(&nor->mtd, NULL, &ppdata, NULL, 0);
}

static int altera_quadspi_probe(struct platform_device *pdev)
{
   struct device_node *np = pdev->dev.of_node;
   struct device *dev = &pdev->dev;
   struct altera_quadspi *q;
   int ret = 0;
   int i;
   int count;

   if (!np) {
       dev_err(dev, "no device found\n");
       return -ENODEV;
   }

   q = devm_kzalloc(dev, sizeof(*q), GFP_KERNEL);
   if (!q)
       return -ENOMEM;

   ret = altera_quadspi_probe_config_dt(pdev, np, q);
   if (ret) {
       dev_err(dev, "probe device tree failed\n");
       return -ENODEV;
   }

   q->dev = dev;

   /* check number of flashes */
   if (q->num_flashes > MAX_NUM_FLASH_CHIP) {
       dev_err(dev, "exceeding max number of flashes\n");
       q->num_flashes = MAX_NUM_FLASH_CHIP;
   }

   platform_set_drvdata(pdev, q);

   /* count is number of successful setup chips */
   count = q->num_flashes;
   /* loop for each serial flash which is connected to quadspi */
   for (i = 0; i < q->num_flashes; i++) {
       ret = altera_quadspi_setup_banks(pdev, i, q->np[i]);
       if (ret) {
           dev_err(dev, "bank %d setup failed\n", i);
           count--;
       }
   }
   return (count > 0) ? 0 : -ENODEV;
}

static int altera_quadspi_remove(struct platform_device *pdev)
{
   struct altera_quadspi *q = platform_get_drvdata(pdev);
   struct altera_quadspi_flash *flash;
   int i;
   int ret = 0;

   /* clean up for all nor flash */
   for (i = 0; i < q->num_flashes; i++) {
       flash = q->flash[i];
       if (!flash)
           continue;

       /* clean up mtd stuff */
       ret = mtd_device_unregister(&flash->nor.mtd);
       if (ret) {
           dev_err(&pdev->dev, "error removing mtd\n");
           return ret;
       }
   }

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
MODULE_DESCRIPTION("Altera QuadSPI Driver");
MODULE_LICENSE("GPL v2");
