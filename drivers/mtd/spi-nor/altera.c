// SPDX-License-Identifier: GPL-2.0-only

#include <linux/mtd/spi-nor.h>

#include "core.h"

static const struct flash_info altera_parts[] = {
	/* Altera EPCQ Flashes are non-JEDEC */
	{ "epcq16",	INFO(0, 0, 0x10000, 32,   0) },
	{ "epcq32",	INFO(0, 0, 0x10000, 64,   0) },
	{ "epcq64",	INFO(0, 0, 0x10000, 128,  0) },
	{ "epcq128",	INFO(0, 0, 0x10000, 256,  0) },
	{ "epcq256",	INFO(0, 0, 0x10000, 512,  0) },
	{ "epcq512",	INFO(0, 0, 0x10000, 1024, 0) },
	{ "epcq1024",	INFO(0, 0, 0x10000, 2048, 0) },
	{ "epcql256",	INFO(0, 0, 0x10000, 512,  0) },
	{ "epcql512",	INFO(0, 0, 0x10000, 1024, 0) },
	{ "epcql1024",	INFO(0, 0, 0x10000, 2048, 0) },
};

const struct spi_nor_manufacturer spi_nor_altera = {
	.name = "altera",
	.parts = altera_parts,
	.nparts = ARRAY_SIZE(altera_parts),
};
