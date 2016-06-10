#ifndef __MSGDMAHW_H__
#define __MSGDMAHW_H__

/*
 * Altera Modular Scatter-Gather DMA register defines
 * https://www.altera.com/en_US/pdfs/literature/ug/ug_embedded_ip.pdf
 */

/*
 * The control and status register port is read/write accessible and is 32 bits
 * wide. When the dispatcher response port is disabled or set to memory-mapped
 * mode then the CSR port is responsible for sending interrupts to the host.
 */
struct msgdma_csr_regs {
	uint32_t status;
#define MSGDMA_CSR_BUSY			(1 << 0)
#define MSGDMA_CSR_DESC_BUF_EMPTY	(1 << 1)
#define MSGDMA_CSR_DESC_BUF_FULL	(1 << 2)
#define MSGDMA_CSR_RESP_BUF_EMPTY	(1 << 3)
#define MSGDMA_CSR_RESP_BUF_FULL	(1 << 4)
#define MSGDMA_CSR_STOPPED		(1 << 5)
#define MSGDMA_CSR_RESETTING		(1 << 6)
#define MSGDMA_CSR_STOPPED_ON_ERR	(1 << 7)
#define MSGDMA_CSR_STOPPED_ON_EARLY_TERM (1 << 8)
#define MSGDMA_CSR_IRQ			(1 << 9)
	uint32_t control;
#define MSGDMA_CSR_STOP_DISP		(1 << 0)
#define MSGDMA_CSR_RESET_DISP		(1 << 1)
#define MSGDMA_CSR_STOP_ON_ERR		(1 << 2)
#define MSGDMA_CSR_STOP_ON_EARLY_TERM	(1 << 3)
#define MSGDMA_CSR_IRQ_EN		(1 << 4)
	uint32_t rw_fill_level;
	uint32_t resp_fill_level;
	uint32_t rw_seq_number;
	uint32_t pad[3];
};

/*
 * The descriptor slave port is write only and configurable to either 128 or
 * 256 bits wide. The width is dependent on the descriptor format you choose to
 * use in your system. It is important to note that when writing descriptors to
 * this port, you must set the last bit high for the descriptor to be
 * completely written to the dispatcher module. You can access the byte lanes
 * of this port in any order as long as the last bit is written to during the
 * last write access.
 */
struct msgdma_desc_regs {
	uint32_t read_addr;	/* data source address */
	uint32_t write_addr;	/* data destination address */
	uint32_t length;	/* number of bytes to transfer per descriptor */
	uint32_t control;
#define MSGDMA_DESC_END_ON_EOP		(1 << 12)
#define MSGDMA_DESC_END_ON_LEN		(1 << 13)
#define MSGDMA_DESC_TX_IRQ_EN		(1 << 14)
#define MSGDMA_DESC_EARLY_TERM_IRQ_EN	(1 << 15)
#define MSGDMA_DESC_ERR_IRQ_EN		(0xff << 16)
#define MSGDMA_DESC_GO			(1 << 31)
};

/*
 * mSGDMA response register map
 *
 * The response slave port of the mSGDMA contains registers providing
 * information  of the executed transaction. This register map is only
 * applicable when the response mode is enabled and set to MM. Also when the
 * response port is enabled it needs to have responses read because it buffers
 * responses. When it is setup as a memory-mapped slave port, reading byte
 * offset 0x7 pops the response. If the response FIFO becomes full the
 * dispatcher stops issuing transfer commands to the read and write masters.
 */
struct msgdma_response {
	uint32_t bytes_transferred;
	uint32_t status;
};

#endif /* __MSGDMAHW_H__*/

/* vim: set sw=8 ts=8 noexpandtab: */
