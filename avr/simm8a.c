/*
 * mcusim - Interactive simulator for microcontrollers.
 * Copyright (C) 2017 Dmitry Salychev <darkness.bsd@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
#include <stdio.h>
#include <string.h>

/*
 * We would like to include headers specific to the
 * ATMega8A microcontroller.
 */
#define __AVR_ATmega8A__ 1

#include "avr/io.h"
#include "avr/sim/sim.h"
#include "tools/gis/ihex.h"

static int set_fuse_bytes(struct avr *mcu, uint8_t high, uint8_t low);
static int is_ckopt_programmed(uint8_t ckopt_f);

/*
 * Set ATmega8A lock bits to the default values
 * according to the datasheet. ATmega8A has 6 lock bits only.
 *
 * They are:
 * 5     4     3     2     1   0
 * BLB12 BLB11 BLB02 BLB01 LB2 LB1
 *
 * Default value means:
 *	- no memory lock features enabled;
 *	- no restrictions for SPM or Load Program Memory (LPM)
 *	  instruction accessing the Application section;
 *	- no restrictions for SPM or LPM accessing
 *	  the Boot Loader section.
 *
 * Set ATmega8A fuse bits to the default values. ATmega8A has
 * only two of them, high and low.
 *
 * The high fuse byte is:
 * 7        6     5     4     3      2       1       0
 * RSTDISBL WDTON SPIEN CKOPT EESAVE BOOTSZ1 BOOTSZ0 BOOTRST
 *
 * The low fuse byte is:
 * 7        6     5    4    3      2      1      0
 * BODLEVEL BODEN SUT1 SUT0 CKSEL3 CKSEL2 CKSEL1 CKSEL0
 *
 * Default value for high byte means:
 *	- boot sector in program memory is 1024 words,
 *	  from 0xC00-0xFFF,
 *	  application sector is 3072 words,
 *	  from 0x000-0xBFF;
 *	- ...
 *
 * Default value for low byte means:
 *	- ...
 */
int m8a_init(struct avr *mcu)
{
	if (!mcu) {
		fprintf(stderr, "MCU should not be NULL\n");
		return -1;
	}

	strcpy(mcu->name, "atmega8a");

	/*
	 * Set values according to the header file included
	 * by avr/io.h.
	 */
	mcu->spm_pagesize = SPM_PAGESIZE;
	mcu->flashstart = FLASHSTART;
	mcu->flashend = FLASHEND;

	mcu->ramstart = RAMSTART;
	mcu->ramend = RAMEND;
	mcu->ramsize = RAMSIZE;

	mcu->e2start = E2START;
	mcu->e2end = E2END;
	mcu->e2size = E2SIZE;
	mcu->e2pagesize = E2PAGESIZE;

	mcu->lockbits = 0x3F;

	if (set_fuse_bytes(mcu, 0xD9, 0xE1)) {
		fprintf(stderr, "Fuse bytes cannot be set correctly\n");
		return -1;
	}

	return 0;
}

int m8a_set_progmem(struct avr *mcu, uint16_t *mem, uint32_t size)
{
	uint16_t flash_size;

	/* Size in 16-bits words */
	flash_size= (uint16_t) ((mcu->flashend - mcu->flashstart) + 1) / 2;
	if (size != flash_size) {
		fprintf(stderr, "Program memory is limited by %d KiB,"
				" %u.%03u KiB doesn't match",
				(mcu->flashend + 1) / 1024,
				(size * 2) / 1024, (size * 2) % 1024);
		return -1;
	}

	mcu->prog_mem = mem;

	return 0;
}

int m8a_load_progmem(struct avr *mcu, FILE *fp)
{
	IHexRecord rec, mem_rec;

	if (!fp) {
		fprintf(stderr, "Cannot read from the filestream");
		return -1;
	}

	/*
	 * Copy HEX data to program memory of the MCU.
	 */
	while (Read_IHexRecord(&rec, fp) == IHEX_OK) {
		switch (rec.type) {
		case IHEX_TYPE_00:	/* Data */
			memcpy(mcu->prog_mem + (rec.address / 2),
			       rec.data, (uint16_t) rec.dataLen);
			break;
		case IHEX_TYPE_01:	/* End of File */
		default:		/* Other types, unlikely occured */
			continue;
		}
	}

	/*
	 * Verify checksum of the loaded data.
	 */
	rewind(fp);
	while (Read_IHexRecord(&rec, fp) == IHEX_OK) {
		if (rec.type != IHEX_TYPE_00)
			continue;

		memcpy(mem_rec.data, mcu->prog_mem + (rec.address / 2),
		       (uint16_t) rec.dataLen);
		mem_rec.address = rec.address;
		mem_rec.dataLen = rec.dataLen;
		mem_rec.type = rec.type;
		mem_rec.checksum = 0;

		mem_rec.checksum = Checksum_IHexRecord(&mem_rec);
		if (mem_rec.checksum != rec.checksum) {
			printf("Checksum is not correct:"
			       " 0x%x (memory) != 0x%x (file)\n"
			       "File record:\n", mem_rec.checksum, rec.checksum);
			Print_IHexRecord(&rec);
			printf("Memory record:\n");
			Print_IHexRecord(&mem_rec);
			return -1;
		}
	}
	return 0;
}

int m8a_set_datamem(struct avr *mcu, uint8_t *mem, uint32_t size)
{
	return -1;
}

static int set_fuse_bytes(struct avr *mcu, uint8_t high, uint8_t low)
{
	uint8_t bldr_f, cksel_f, ckopt_f;

	mcu->fuse[1] = high;
	mcu->fuse[0] = low;

	/*
	 * Check BOOTSZ1:0 flags and set bootloader
	 * parameters accordingly.
	 */
	bldr_f = (high >> 1) & 0x03;
	switch (bldr_f) {
	case 0x01:
		mcu->boot_loader->start = 0xE00;
		mcu->boot_loader->end = 0xFFF;
		mcu->boot_loader->size = 512;
		break;
	case 0x02:
		mcu->boot_loader->start = 0xF00;
		mcu->boot_loader->end = 0xFFF;
		mcu->boot_loader->size = 256;
		break;
	case 0x03:
		mcu->boot_loader->start = 0xF80;
		mcu->boot_loader->end = 0xFFF;
		mcu->boot_loader->size = 128;
		break;
	case 0x00:
	default:
		mcu->boot_loader->start = 0xC00;
		mcu->boot_loader->end = 0xFFF;
		mcu->boot_loader->size = 1024;
		break;
	}

	/*
	 * Check CKOPT and CKSEL3:0 in order to understand where
	 * clock signal comes from and expected frequency.
	 *
	 * The default option for ATmega8A is 1MHz internal RC oscillator.
	 * CKOPT should always be unprogrammed (value is 1) when using
	 * internal oscillator.
	 */
	ckopt_f = (high >> 4) & 0x01;
	cksel_f = low & 0x0F;
	switch(cksel_f) {
	case 0x02:
		/* Internal, 2 MHz */
		if (is_ckopt_programmed(ckopt_f))
			return -1;
		mcu->clk_source = AVR_INT_CLK;
		mcu->freq = 2000;
		break;
	case 0x03:
		/* Internal, 4 MHz */
		if (is_ckopt_programmed(ckopt_f))
			return -1;
		mcu->clk_source = AVR_INT_CLK;
		mcu->freq = 4000;
		break;
	case 0x04:
		/* Internal, 8 MHz */
		if (is_ckopt_programmed(ckopt_f))
			return -1;
		mcu->clk_source = AVR_INT_CLK;
		mcu->freq = 8000;
		break;
	case 0x01:
	default:
		/* Internal, 1 MHz */
		if (is_ckopt_programmed(ckopt_f))
			return -1;
		mcu->clk_source = AVR_INT_CLK;
		mcu->freq = 1000;
		break;
	case 0x00:
		/*
		 * External Clock
		 *
		 * It is not meant to be a crystal/ceramic resonator,
		 * crystal oscillator or RC oscillator, so we cannot
		 * expect any frequency.
		 */
		mcu->clk_source = AVR_EXT_CLK;
		mcu->freq = UINT32_MAX;
		break;
	}

	return 0;
}

static int is_ckopt_programmed(uint8_t ckopt_f)
{
	if (!ckopt_f) {
		fprintf(stderr, "CKOPT fuse bit should be unprogrammed "
				"(CKOPT == 1) using internal clock source\n");
		return -1;
	}
	return 0;
}
