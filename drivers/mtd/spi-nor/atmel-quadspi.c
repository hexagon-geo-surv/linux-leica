/*
 * Driver for Atmel QSPI Controller
 *
 * Copyright (C) 2015 Atmel Corporation
 *
 * Author: Cyrille Pitchen <cyrille.pitchen@atmel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This driver is based on drivers/mtd/spi-nor/fsl-quadspi.c from Freescale.
 */

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/spi-nor.h>
#include <linux/platform_data/atmel.h>
#include <linux/platform_data/dma-atmel.h>
#include <linux/of.h>

#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>

/* QSPI register offsets */
#define QSPI_CR      0x0000  /* Control Register */
#define QSPI_MR      0x0004  /* Mode Register */
#define QSPI_RD      0x0008  /* Receive Data Register */
#define QSPI_TD      0x000c  /* Transmit Data Register */
#define QSPI_SR      0x0010  /* Status Register */
#define QSPI_IER     0x0014  /* Interrupt Enable Register */
#define QSPI_IDR     0x0018  /* Interrupt Disable Register */
#define QSPI_IMR     0x001c  /* Interrupt Mask Register */
#define QSPI_SCR     0x0020  /* Serial Clock Register */

#define QSPI_IAR     0x0030  /* Instruction Address Register */
#define QSPI_ICR     0x0034  /* Instruction Code Register */
#define QSPI_IFR     0x0038  /* Instruction Frame Register */

#define QSPI_SMR     0x0040  /* Scrambling Mode Register */
#define QSPI_SKR     0x0044  /* Scrambling Key Register */

#define QSPI_WPMR    0x00E4  /* Write Protection Mode Register */
#define QSPI_WPSR    0x00E8  /* Write Protection Status Register */

#define QSPI_VERSION 0x00FC  /* Version Register */


/* Bitfields in QSPI_CR (Control Register) */
#define QSPI_CR_QSPIEN                  BIT(0)
#define QSPI_CR_QSPIDIS                 BIT(1)
#define QSPI_CR_SWRST                   BIT(7)
#define QSPI_CR_LASTXFER                BIT(24)

/* Bitfields in QSPI_MR (Mode Register) */
#define QSPI_MR_SSM                     BIT(0)
#define QSPI_MR_LLB                     BIT(1)
#define QSPI_MR_WDRBT                   BIT(2)
#define QSPI_MR_SMRM                    BIT(3)
#define QSPI_MR_CSMODE_MASK             GENMASK(5, 4)
#define QSPI_MR_CSMODE_NOT_RELOADED     (0 << 4)
#define QSPI_MR_CSMODE_LASTXFER         (1 << 4)
#define QSPI_MR_CSMODE_SYSTEMATICALLY   (2 << 4)
#define QSPI_MR_NBBITS_MASK             GENMASK(11, 8)
#define QSPI_MR_NBBITS(n)               ((((n) - 8) << 8) & QSPI_MR_NBBITS_MASK)
#define QSPI_MR_DLYBCT_MASK             GENMASK(23, 16)
#define QSPI_MR_DLYBCT(n)               (((n) << 16) & QSPI_MR_DLYBCT_MASK)
#define QSPI_MR_DLYCS_MASK              GENMASK(31, 24)
#define QSPI_MR_DLYCS(n)                (((n) << 24) & QSPI_MR_DLYCS_MASK)

/* Bitfields in QSPI_SR/QSPI_IER/QSPI_IDR/QSPI_IMR  */
#define QSPI_SR_RDRF                    BIT(0)
#define QSPI_SR_TDRE                    BIT(1)
#define QSPI_SR_TXEMPTY                 BIT(2)
#define QSPI_SR_OVRES                   BIT(3)
#define QSPI_SR_CSR                     BIT(8)
#define QSPI_SR_CSS                     BIT(9)
#define QSPI_SR_INSTRE                  BIT(10)
#define QSPI_SR_QSPIENS                 BIT(24)

/* Bitfields in QSPI_SCR (Serial Clock Register) */
#define QSPI_SCR_CPOL                   BIT(0)
#define QSPI_SCR_CPHA                   BIT(1)
#define QSPI_SCR_SCBR_MASK              GENMASK(15, 8)
#define QSPI_SCR_SCBR(n)                (((n) << 8) & QSPI_SCR_SCBR_MASK)
#define QSPI_SCR_DLYBS_MASK             GENMASK(23, 16)
#define QSPI_SCR_DLYBS(n)               (((n) << 16) & QSPI_SCR_DLYBS_MASK)

/* Bitfields in QSPI_ICR (Instruction Code Register) */
#define QSPI_ICR_INST_MASK              GENMASK(7, 0)
#define QSPI_ICR_INST(inst)             (((inst) << 0) & QSPI_ICR_INST_MASK)
#define QSPI_ICR_OPT_MASK               GENMASK(23, 16)
#define QSPI_ICR_OPT(opt)               (((opt) << 16) & QSPI_ICR_OPT_MASK)

/* Bitfields in QSPI_IFR (Instruction Frame Register) */
#define QSPI_IFR_WIDTH_MASK             GENMASK(2, 0)
#define QSPI_IFR_WIDTH_SINGLE_BIT_SPI   (0 << 0)
#define QSPI_IFR_WIDTH_DUAL_OUTPUT      (1 << 0)
#define QSPI_IFR_WIDTH_QUAD_OUTPUT      (2 << 0)
#define QSPI_IFR_WIDTH_DUAL_IO          (3 << 0)
#define QSPI_IFR_WIDTH_QUAD_IO          (4 << 0)
#define QSPI_IFR_WIDTH_DUAL_CMD         (5 << 0)
#define QSPI_IFR_WIDTH_QUAD_CMD         (6 << 0)
#define QSPI_IFR_INSTEN                 BIT(4)
#define QSPI_IFR_ADDREN                 BIT(5)
#define QSPI_IFR_OPTEN                  BIT(6)
#define QSPI_IFR_DATAEN                 BIT(7)
#define QSPI_IFR_OPTL_MASK              GENMASK(9, 8)
#define QSPI_IFR_OPTL_1BIT              (0 << 8)
#define QSPI_IFR_OPTL_2BIT              (1 << 8)
#define QSPI_IFR_OPTL_4BIT              (2 << 8)
#define QSPI_IFR_OPTL_8BIT              (3 << 8)
#define QSPI_IFR_ADDRL                  BIT(10)
#define QSPI_IFR_TFRTYP_MASK            GENMASK(13, 12)
#define QSPI_IFR_TFRTYP_TRSFR_READ      (0 << 12)
#define QSPI_IFR_TFRTYP_TRSFR_READ_MEM  (1 << 12)
#define QSPI_IFR_TFRTYP_TRSFR_WRITE     (2 << 12)
#define QSPI_IFR_TFRTYP_TRSFR_WRITE_MEM (3 << 13)
#define QSPI_IFR_CRM                    BIT(14)
#define QSPI_IFR_NBDUM_MASK             GENMASK(20, 16)
#define QSPI_IFR_NBDUM(n)               (((n) << 16) & QSPI_IFR_NBDUM_MASK)

/* Bitfields in QSPI_SMR (Scrambling Mode Register) */
#define QSPI_SMR_SCREN                  BIT(0)
#define QSPI_SMR_RVDIS                  BIT(1)

/* Bitfields in QSPI_WPMR (Write Protection Mode Register) */
#define QSPI_WPMR_WPEN                  BIT(0)
#define QSPI_WPMR_WPKEY_MASK            GENMASK(31, 8)
#define QSPI_WPMR_WPKEY(wpkey)          (((wpkey) << 8) & QSPI_WPMR_WPKEY_MASK)

/* Bitfields in QSPI_WPSR (Write Protection Status Register) */
#define QSPI_WPSR_WPVS                  BIT(0)
#define QSPI_WPSR_WPVSRC_MASK           GENMASK(15, 8)
#define QSPI_WPSR_WPVSRC(src)           (((src) << 8) & QSPI_WPSR_WPVSRC)


struct atmel_qspi {
	void __iomem		*regs;
	void __iomem		*mem;
	dma_addr_t		phys_addr;
	struct dma_chan		*chan;
	struct clk		*clk;
	struct platform_device	*pdev;
	u32			pending;

	struct spi_nor		nor;
	u32			clk_rate;
	struct completion	cmd_completion;
	struct completion	dma_completion;

#ifdef DEBUG
	u8			last_instruction;
#endif
};

struct atmel_qspi_command {
	u32	ifr;
	union {
		struct {
			u32	instruction:1;
			u32	address:3;
			u32	mode:1;
			u32	dummy:1;
			u32	data:1;
			u32	dma:1;
			u32	reserved:24;
		}		bits;
		u32	word;
	}	enable;
	u8	instruction;
	u8	mode;
	u8	num_mode_cycles;
	u8	num_dummy_cycles;
	u32	address;

	size_t		buf_len;
	const void	*tx_buf;
	void		*rx_buf;
};

/* Register access functions */
static inline u32 qspi_readl(struct atmel_qspi *aq, u32 reg)
{
	return readl_relaxed(aq->regs + reg);
}

static inline void qspi_writel(struct atmel_qspi *aq, u32 reg, u32 value)
{
	writel_relaxed(value, aq->regs + reg);
}


#define QSPI_DMA_THRESHOLD	32

static void atmel_qspi_dma_callback(void *arg)
{
	struct completion *dma_completion = arg;

	complete(dma_completion);
}

static int atmel_qspi_run_dma_transfer(struct atmel_qspi *aq,
				       const struct atmel_qspi_command *cmd)
{
	u32 offset = (cmd->enable.bits.address) ? cmd->address : 0;
	struct dma_chan *chan = aq->chan;
	struct device *dev = &aq->pdev->dev;
	enum dma_data_direction direction;
	dma_addr_t phys_addr, dst, src;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	int err = 0;

	if (cmd->tx_buf) {
		direction = DMA_TO_DEVICE;
		phys_addr = dma_map_single(dev, (void *)cmd->tx_buf,
					   cmd->buf_len, direction);
		src = phys_addr;
		dst = aq->phys_addr + offset;
	} else {
		direction = DMA_FROM_DEVICE;
		phys_addr = dma_map_single(dev, (void *)cmd->rx_buf,
					   cmd->buf_len, direction);
		src = aq->phys_addr + offset;
		dst = phys_addr;
	}
	if (dma_mapping_error(dev, phys_addr))
		return -ENOMEM;

	desc = chan->device->device_prep_dma_memcpy(chan, dst, src,
						    cmd->buf_len,
						    DMA_PREP_INTERRUPT);
	if (!desc) {
		err = -ENOMEM;
		goto unmap_single;
	}

	reinit_completion(&aq->dma_completion);
	desc->callback = atmel_qspi_dma_callback;
	desc->callback_param = &aq->dma_completion;
	cookie = dmaengine_submit(desc);
	err = dma_submit_error(cookie);
	if (err)
		goto unmap_single;
	dma_async_issue_pending(chan);

	if (!wait_for_completion_timeout(&aq->dma_completion,
					 msecs_to_jiffies(1000)))
		err = -ETIMEDOUT;

	if (dma_async_is_tx_complete(chan, cookie, NULL, NULL) != DMA_COMPLETE)
		err = -ETIMEDOUT;

	if (err)
		dmaengine_terminate_all(chan);
unmap_single:
	dma_unmap_single(dev, phys_addr, cmd->buf_len, direction);

	return err;
}

static int atmel_qspi_run_transfer(struct atmel_qspi *aq,
				   const struct atmel_qspi_command *cmd)
{
	void __iomem *ahb_mem;

	/* First try a DMA transfer */
	if (aq->chan && cmd->enable.bits.dma &&
	    cmd->buf_len >= QSPI_DMA_THRESHOLD)
		return atmel_qspi_run_dma_transfer(aq, cmd);

	/* Then fallback to a PIO transfer (memcpy() DOES NOT work!) */
	ahb_mem = aq->mem;
	if (cmd->enable.bits.address)
		ahb_mem += cmd->address;
	if (cmd->tx_buf)
		_memcpy_toio(ahb_mem, cmd->tx_buf, cmd->buf_len);
	else
		_memcpy_fromio(cmd->rx_buf, ahb_mem, cmd->buf_len);

	return 0;
}

#ifdef DEBUG
static void atmel_qspi_debug_command(struct atmel_qspi *aq,
				     const struct atmel_qspi_command *cmd)
{
	u8 cmd_buf[SPI_NOR_MAX_CMD_SIZE];
	size_t len = 0;
	int i;

	if (cmd->enable.bits.instruction) {
		if (aq->last_instruction == cmd->instruction)
			return;
		aq->last_instruction = cmd->instruction;
	}

	if (cmd->enable.bits.instruction)
		cmd_buf[len++] = cmd->instruction;

	for (i = cmd->enable.bits.address-1; i >= 0; --i)
		cmd_buf[len++] = (cmd->address >> (i << 3)) & 0xff;

	if (cmd->enable.bits.mode)
		cmd_buf[len++] = cmd->mode;

	if (cmd->enable.bits.dummy) {
		int num = cmd->num_dummy_cycles;

		switch (cmd->ifr & QSPI_IFR_WIDTH_MASK) {
		case QSPI_IFR_WIDTH_SINGLE_BIT_SPI:
		case QSPI_IFR_WIDTH_DUAL_OUTPUT:
		case QSPI_IFR_WIDTH_QUAD_OUTPUT:
			num >>= 3;
			break;
		case QSPI_IFR_WIDTH_DUAL_IO:
		case QSPI_IFR_WIDTH_DUAL_CMD:
			num >>= 2;
			break;
		case QSPI_IFR_WIDTH_QUAD_IO:
		case QSPI_IFR_WIDTH_QUAD_CMD:
			num >>= 1;
			break;
		default:
			return;
		}

		for (i = 0; i < num; ++i)
			cmd_buf[len++] = 0;
	}

	/* Dump the SPI command */
	print_hex_dump(KERN_DEBUG, "qspi cmd: ", DUMP_PREFIX_NONE,
		       32, 1, cmd_buf, len, false);

#ifdef VERBOSE_DEBUG
	/* If verbose debug is enabled, also dump the TX data */
	if (cmd->enable.bits.data && cmd->tx_buf)
		print_hex_dump(KERN_DEBUG, "qspi tx : ", DUMP_PREFIX_NONE,
			       32, 1, cmd->tx_buf, cmd->buf_len, false);
#endif
}
#else
#define atmel_qspi_debug_command(aq, cmd)
#endif

static int atmel_qspi_run_command(struct atmel_qspi *aq,
				  const struct atmel_qspi_command *cmd)
{
	u32 iar, icr, ifr, sr;
	int err = 0;

	iar = 0;
	icr = 0;
	ifr = cmd->ifr;

	/* Compute instruction parameters */
	if (cmd->enable.bits.instruction) {
		icr |= QSPI_ICR_INST(cmd->instruction);
		ifr |= QSPI_IFR_INSTEN;
	}

	/* Compute address parameters */
	switch (cmd->enable.bits.address) {
	case 4:
		ifr |= QSPI_IFR_ADDRL;
		/* fall through to the 24bit (3 byte) address case. */
	case 3:
		iar = (cmd->enable.bits.data) ? 0 : cmd->address;
		ifr |= QSPI_IFR_ADDREN;
		break;
	case 0:
		break;
	default:
		return -EINVAL;
	}

	/* Compute option parameters */
	if (cmd->enable.bits.mode && cmd->num_mode_cycles) {
		u32 mode_cycle_bits, mode_bits;

		icr |= QSPI_ICR_OPT(cmd->mode);
		ifr |= QSPI_IFR_OPTEN;

		switch (ifr & QSPI_IFR_WIDTH_MASK) {
		case QSPI_IFR_WIDTH_SINGLE_BIT_SPI:
		case QSPI_IFR_WIDTH_DUAL_OUTPUT:
		case QSPI_IFR_WIDTH_QUAD_OUTPUT:
			mode_cycle_bits = 1;
			break;
		case QSPI_IFR_WIDTH_DUAL_IO:
		case QSPI_IFR_WIDTH_DUAL_CMD:
			mode_cycle_bits = 2;
			break;
		case QSPI_IFR_WIDTH_QUAD_IO:
		case QSPI_IFR_WIDTH_QUAD_CMD:
			mode_cycle_bits = 4;
			break;
		default:
			return -EINVAL;
		}

		mode_bits = cmd->num_mode_cycles * mode_cycle_bits;
		switch (mode_bits) {
		case 1:
			ifr |= QSPI_IFR_OPTL_1BIT;
			break;

		case 2:
			ifr |= QSPI_IFR_OPTL_2BIT;
			break;

		case 4:
			ifr |= QSPI_IFR_OPTL_4BIT;
			break;

		case 8:
			ifr |= QSPI_IFR_OPTL_8BIT;
			break;

		default:
			return -EINVAL;
		}
	}

	/* Set number of dummy cycles */
	if (cmd->enable.bits.dummy)
		ifr |= QSPI_IFR_NBDUM(cmd->num_dummy_cycles);

	/* Set data enable */
	if (cmd->enable.bits.data) {
		ifr |= QSPI_IFR_DATAEN;

		/* Special case for Continuous Read Mode */
		if (!cmd->tx_buf && !cmd->rx_buf)
			ifr |= QSPI_IFR_CRM;
	}

	/* Set QSPI Instruction Frame registers */
	atmel_qspi_debug_command(aq, cmd);
	qspi_writel(aq, QSPI_IAR, iar);
	qspi_writel(aq, QSPI_ICR, icr);
	qspi_writel(aq, QSPI_IFR, ifr);

	/* Skip to the final steps if there is no data */
	if (!cmd->enable.bits.data)
		goto no_data;

	/* Dummy read of QSPI_IFR to synchronize APB and AHB accesses */
	(void)qspi_readl(aq, QSPI_IFR);

	/* Stop here for continuous read */
	if (!cmd->tx_buf && !cmd->rx_buf)
		return 0;
	/* Send/Receive data */
	err = atmel_qspi_run_transfer(aq, cmd);

	/* Release the chip-select */
	qspi_writel(aq, QSPI_CR, QSPI_CR_LASTXFER);

	if (err)
		return err;

#if defined(DEBUG) && defined(VERBOSE_DEBUG)
	/*
	 * If verbose debug is enabled, also dump the RX data in addition to
	 * the SPI command previously dumped by atmel_qspi_debug_command()
	 */
	if (cmd->rx_buf)
		print_hex_dump(KERN_DEBUG, "qspi rx : ", DUMP_PREFIX_NONE,
			       32, 1, cmd->rx_buf, cmd->buf_len, false);
#endif
no_data:
	/* Poll INSTRuction End status */
	sr = qspi_readl(aq, QSPI_SR);
	if (sr & QSPI_SR_INSTRE)
		return err;

	/* Wait for INSTRuction End interrupt */
	reinit_completion(&aq->cmd_completion);
	aq->pending = 0;
	qspi_writel(aq, QSPI_IER, QSPI_SR_INSTRE);
	if (!wait_for_completion_timeout(&aq->cmd_completion,
					 msecs_to_jiffies(1000)))
		err = -ETIMEDOUT;
	qspi_writel(aq, QSPI_IDR, QSPI_SR_INSTRE);

	return err;
}

static int atmel_qspi_command_set_ifr(struct atmel_qspi_command *cmd,
				      u32 ifr_tfrtyp,
				      enum spi_nor_protocol proto)
{
	cmd->ifr = ifr_tfrtyp;

	switch (proto) {
	case SNOR_PROTO_1_1_1:
		cmd->ifr |= QSPI_IFR_WIDTH_SINGLE_BIT_SPI;
		break;
	case SNOR_PROTO_1_1_2:
		cmd->ifr |= QSPI_IFR_WIDTH_DUAL_OUTPUT;
		break;
	case SNOR_PROTO_1_1_4:
		cmd->ifr |= QSPI_IFR_WIDTH_QUAD_OUTPUT;
		break;
	case SNOR_PROTO_1_2_2:
		cmd->ifr |= QSPI_IFR_WIDTH_DUAL_IO;
		break;
	case SNOR_PROTO_1_4_4:
		cmd->ifr |= QSPI_IFR_WIDTH_QUAD_IO;
		break;
	case SNOR_PROTO_2_2_2:
		cmd->ifr |= QSPI_IFR_WIDTH_DUAL_CMD;
		break;
	case SNOR_PROTO_4_4_4:
		cmd->ifr |= QSPI_IFR_WIDTH_QUAD_CMD;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int atmel_qspi_read_reg(struct spi_nor *nor, u8 opcode,
			       u8 *buf, int len)
{
	struct atmel_qspi *aq = nor->priv;
	struct atmel_qspi_command cmd;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.enable.bits.instruction = 1;
	cmd.enable.bits.data = 1;
	cmd.instruction = opcode;
	cmd.rx_buf = buf;
	cmd.buf_len = len;

	ret = atmel_qspi_command_set_ifr(&cmd,
					 QSPI_IFR_TFRTYP_TRSFR_READ,
					 nor->reg_proto);
	if (ret)
		return ret;

	return atmel_qspi_run_command(aq, &cmd);
}

static int atmel_qspi_write_reg(struct spi_nor *nor, u8 opcode,
				u8 *buf, int len)
{
	struct atmel_qspi *aq = nor->priv;
	struct atmel_qspi_command cmd;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.enable.bits.instruction = 1;
	cmd.enable.bits.data = (buf != NULL && len > 0);
	cmd.instruction = opcode;
	cmd.tx_buf = buf;
	cmd.buf_len = len;

	ret = atmel_qspi_command_set_ifr(&cmd,
					 QSPI_IFR_TFRTYP_TRSFR_WRITE,
					 nor->reg_proto);
	if (ret)
		return ret;

	return atmel_qspi_run_command(aq, &cmd);
}

static void atmel_qspi_write(struct spi_nor *nor, loff_t to, size_t len,
			     size_t *retlen, const u_char *write_buf)
{
	struct atmel_qspi *aq = nor->priv;
	struct atmel_qspi_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.enable.bits.instruction = 1;
	cmd.enable.bits.address = nor->addr_width;
	cmd.enable.bits.data = 1;
	cmd.enable.bits.dma = 1;
	cmd.instruction = nor->program_opcode;
	cmd.address = (u32)to;
	cmd.tx_buf = write_buf;
	cmd.buf_len = len;

	if (atmel_qspi_command_set_ifr(&cmd,
				       QSPI_IFR_TFRTYP_TRSFR_WRITE_MEM,
				       nor->write_proto))
		return;

	if (!atmel_qspi_run_command(aq, &cmd))
		*retlen += len;
}

static int atmel_qspi_erase(struct spi_nor *nor, loff_t offs)
{
	struct atmel_qspi *aq = nor->priv;
	struct atmel_qspi_command cmd;
	int ret;

	dev_dbg(nor->dev, "%dKiB at 0x%08x\n",
		nor->mtd.erasesize / 1024, (u32)offs);

	memset(&cmd, 0, sizeof(cmd));
	cmd.enable.bits.instruction = 1;
	cmd.enable.bits.address = nor->addr_width;
	cmd.instruction = nor->erase_opcode;
	cmd.address = (u32)offs;

	ret = atmel_qspi_command_set_ifr(&cmd,
					 QSPI_IFR_TFRTYP_TRSFR_WRITE,
					 nor->erase_proto);
	if (ret)
		return ret;

	return atmel_qspi_run_command(aq, &cmd);
}

static int atmel_qspi_read(struct spi_nor *nor, loff_t from, size_t len,
			   size_t *retlen, u_char *read_buf)
{
	struct atmel_qspi *aq = nor->priv;
	struct atmel_qspi_command cmd;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.enable.bits.instruction = 1;
	cmd.enable.bits.address = nor->addr_width;
	cmd.enable.bits.dummy = (nor->read_dummy > 0);
	cmd.enable.bits.data = 1;
	cmd.enable.bits.dma = 1;
	cmd.instruction = nor->read_opcode;
	cmd.address = (u32)from;
	cmd.num_dummy_cycles = nor->read_dummy;
	cmd.rx_buf = read_buf;
	cmd.buf_len = len;

	ret = atmel_qspi_command_set_ifr(&cmd,
					 QSPI_IFR_TFRTYP_TRSFR_READ_MEM,
					 nor->read_proto);
	if (ret)
		return ret;

	ret = atmel_qspi_run_command(aq, &cmd);
	if (ret)
		return ret;

	*retlen += len;
	return 0;
}

static int atmel_qspi_init(struct atmel_qspi *aq)
{
	unsigned long src_rate;
	u32 mr, scr, scbr;

	/* Reset the QSPI controller */
	qspi_writel(aq, QSPI_CR, QSPI_CR_SWRST);

	/* Set the QSPI controller in Serial Memory Mode */
	mr = QSPI_MR_NBBITS(8) | QSPI_MR_SSM;
	qspi_writel(aq, QSPI_MR, mr);

	src_rate = clk_get_rate(aq->clk);
	if (!src_rate)
		return -EINVAL;

	/* Compute the QSPI baudrate */
	scbr = DIV_ROUND_UP(src_rate, aq->clk_rate);
	if (scbr > 0)
		scbr--;
	scr = QSPI_SCR_SCBR(scbr);
	qspi_writel(aq, QSPI_SCR, scr);

	/* Enable the QSPI controller */
	qspi_writel(aq, QSPI_CR, QSPI_CR_QSPIEN);

	return 0;
}

static irqreturn_t atmel_qspi_interrupt(int irq, void *dev_id)
{
	struct atmel_qspi *aq = (struct atmel_qspi *)dev_id;
	u32 status, mask, pending;

	status = qspi_readl(aq, QSPI_SR);
	mask = qspi_readl(aq, QSPI_IMR);
	pending = status & mask;

	if (!pending)
		return IRQ_NONE;

	aq->pending |= pending;
	if (pending & QSPI_SR_INSTRE)
		complete(&aq->cmd_completion);

	return IRQ_HANDLED;
}

static int atmel_qspi_probe(struct platform_device *pdev)
{
	struct device_node *child, *np = pdev->dev.of_node;
	struct atmel_qspi *aq;
	struct resource *res;
	dma_cap_mask_t mask;
	struct spi_nor *nor;
	struct mtd_info *mtd;
	int irq, err = 0;

	if (of_get_child_count(np) != 1)
		return -ENODEV;
	child = of_get_next_child(np, NULL);

	aq = devm_kzalloc(&pdev->dev, sizeof(*aq), GFP_KERNEL);
	if (!aq) {
		err = -ENOMEM;
		goto exit;
	}

	platform_set_drvdata(pdev, aq);
	init_completion(&aq->cmd_completion);
	init_completion(&aq->dma_completion);
	aq->pdev = pdev;

	/* Map the registers */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "qspi_base");
	aq->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(aq->regs)) {
		dev_err(&pdev->dev, "missing registers\n");
		err = PTR_ERR(aq->regs);
		goto exit;
	}

	/* Map the AHB memory */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "qspi_mmap");
	aq->mem = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(aq->mem)) {
		dev_err(&pdev->dev, "missing AHB memory\n");
		err = PTR_ERR(aq->mem);
		goto exit;
	}
	aq->phys_addr = (dma_addr_t)res->start;

	/* Get the peripheral clock */
	aq->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(aq->clk)) {
		dev_err(&pdev->dev, "missing peripheral clock\n");
		err = PTR_ERR(aq->clk);
		goto exit;
	}

	/* Enable the peripheral clock */
	err = clk_prepare_enable(aq->clk);
	if (err) {
		dev_err(&pdev->dev, "failed to enable the peripheral clock\n");
		goto exit;
	}

	/* Request the IRQ */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "missing IRQ\n");
		err = irq;
		goto disable_clk;
	}
	err = devm_request_irq(&pdev->dev, irq, atmel_qspi_interrupt,
			       0, dev_name(&pdev->dev), aq);
	if (err)
		goto disable_clk;

	/* Try to get a DMA channel for memcpy() operation */
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	aq->chan = dma_request_channel(mask, NULL, NULL);
	if (!aq->chan)
		dev_warn(&pdev->dev, "no available DMA channel\n");

	/* Setup the spi-nor */
	nor = &aq->nor;
	mtd = &nor->mtd;

	nor->dev = &pdev->dev;
	spi_nor_set_flash_node(nor, child);
	nor->priv = aq;
	mtd->priv = nor;

	nor->read_reg = atmel_qspi_read_reg;
	nor->write_reg = atmel_qspi_write_reg;
	nor->read = atmel_qspi_read;
	nor->write = atmel_qspi_write;
	nor->erase = atmel_qspi_erase;

	err = of_property_read_u32(child, "spi-max-frequency", &aq->clk_rate);
	if (err < 0)
		goto release_channel;

	err = atmel_qspi_init(aq);
	if (err)
		goto release_channel;

	err = spi_nor_scan(nor, NULL, SPI_NOR_QUAD);
	if (err)
		goto release_channel;

	err = mtd_device_register(mtd, NULL, 0);
	if (err)
		goto release_channel;

	of_node_put(child);

	return 0;

release_channel:
	if (aq->chan)
		dma_release_channel(aq->chan);
disable_clk:
	clk_disable_unprepare(aq->clk);
exit:
	of_node_put(child);

	return err;
}

static int atmel_qspi_remove(struct platform_device *pdev)
{
	struct atmel_qspi *aq = platform_get_drvdata(pdev);

	mtd_device_unregister(&aq->nor.mtd);
	qspi_writel(aq, QSPI_CR, QSPI_CR_QSPIDIS);
	if (aq->chan)
		dma_release_channel(aq->chan);
	clk_disable_unprepare(aq->clk);
	return 0;
}


static const struct of_device_id atmel_qspi_dt_ids[] = {
	{ .compatible = "atmel,sama5d2-qspi" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, atmel_qspi_dt_ids);

static struct platform_driver atmel_qspi_driver = {
	.driver = {
		.name	= "atmel_qspi",
		.of_match_table	= atmel_qspi_dt_ids,
	},
	.probe		= atmel_qspi_probe,
	.remove		= atmel_qspi_remove,
};
module_platform_driver(atmel_qspi_driver);

MODULE_AUTHOR("Cyrille Pitchen <cyrille.pitchen@atmel.com>");
MODULE_DESCRIPTION("Atmel QSPI Controller driver");
MODULE_LICENSE("GPL v2");
