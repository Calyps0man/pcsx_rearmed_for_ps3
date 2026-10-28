/***************************************************************************
 *   Copyright (C) 2010 Gabriele Gorla                                     *
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
 ***************************************************************************/

#include "mdec.h"

/* memory speed is 1 byte per MDEC_BIAS psx clock
 * That mean (PSXCLK / MDEC_BIAS) B/s
 * MDEC_BIAS = 2.0 => ~16MB/s
 * MDEC_BIAS = 3.0 => ~11MB/s
 * and so on ...
 * I guess I have 50 images in 50Hz ... (could be 25 images ?)
 * 320x240x24@50Hz => 11.52 MB/s
 * 320x240x24@60Hz => 13.824 MB/s
 * 320x240x16@50Hz => 7.68 MB/s
 * 320x240x16@60Hz => 9.216 MB/s
 * so 2.0 to 4.0 should be fine.
 */
 
/*
 * >= 14 for Sol Divide
 * <= 18 for "Disney's Treasure Planet"
 * Psychic Detective may break on *any* change
 */
#define MDEC_BIAS 14
#define MDEC_DELAY 1024

#define MDEC_SWAP16(x) SWAP16(x)
#include "mdec_decode.h"


#if 0
static void printmatrixu8(u8 *m) {
	int i;
	for(i = 0; i < DSIZE2; i++) {
		printf("%3d ",m[i]);
		if((i+1) % 8 == 0) printf("\n");
	}
}
#endif

// mdec0: command register
#define MDEC0_STP			0x02000000
#define MDEC0_RGB24			0x08000000
#define MDEC0_SIZE_MASK		0x0000FFFF

// mdec1: status register
#define MDEC1_BUSY			0x20000000
#define MDEC1_DREQ			0x18000000
#define MDEC1_FIFO			0xc0000000
#define MDEC1_RGB24			0x02000000
#define MDEC1_STP			0x00800000
#define MDEC1_RESET			0x80000000

struct _pending_dma1 {
	u32 adr;
	u32 bcr;
	u32 chcr;
};

static struct {
	u32 reg0;
	u32 reg1;
	const u16 * rl;
	const u16 * rl_end;
	u8 * block_buffer_pos;
	u8 block_buffer[16*16*3];
	struct _pending_dma1 pending_dma1;
} mdec;

static int iq_y[DSIZE2], iq_uv[DSIZE2];

#define STP_A ((mdec.reg0 & MDEC0_STP) ? 0x8000 : 0)


static int aanscales[DSIZE2] = {
	1048576, 1454417, 1370031, 1232995, 1048576,  823861, 567485, 289301,
	1454417, 2017334, 1900287, 1710213, 1454417, 1142728, 787125, 401273,
	1370031, 1900287, 1790031, 1610986, 1370031, 1076426, 741455, 377991,
	1232995, 1710213, 1610986, 1449849, 1232995,  968758, 667292, 340183,
	1048576, 1454417, 1370031, 1232995, 1048576,  823861, 567485, 289301,
	823861,  1142728, 1076426, 968758,  823861,  647303, 445870, 227303,
	567485,  787125,  741455,  667292,  567485,  445870, 307121, 156569,
	289301,  401273,  377991,  340183,  289301,  227303, 156569,  79818
};

static void iqtab_init(int *iqtab, const unsigned char *iq_y) {
	int i;

	for (i = 0; i < DSIZE2; i++) {
		iqtab[i] = (iq_y[i] * SCALER(aanscales[zscan[i]], AAN_PRESCALE_SCALE));
	}
}


void mdecInit(void) {
	memset(&mdec, 0, sizeof(mdec));
	memset(iq_y, 0, sizeof(iq_y));
	memset(iq_uv, 0, sizeof(iq_uv));
	mdec.rl = (u16 *)&psxRegs.ptrs.psxM[0x100000];
}

// command register
void mdecWrite0(u32 data) {
	mdec.reg0 = data;
}

u32 mdecRead0(void) {
	return mdec.reg0;
}

// status register
void mdecWrite1(u32 data) {
	if (data & MDEC1_RESET) { // mdec reset
		mdec.reg0 = 0;
		mdec.reg1 = 0;
		mdec.pending_dma1.adr = 0;
		mdec.block_buffer_pos = 0;
	}
}

u32 mdecRead1(void) {
	u32 v = mdec.reg1;
	return v;
}

void psxDma0(u32 adr, u32 bcr, u32 chcr) {
	u32 cmd = mdec.reg0, words_max = 0;
	const void *mem;
	int size;

	if (chcr != 0x01000201) {
		log_unhandled("mdec0: invalid dma %08x\n", chcr);
		return;
	}

	/* mdec is STP till dma0 is released */
	mdec.reg1 |= MDEC1_STP;

	size = (bcr >> 16) * (bcr & 0xffff);

	adr &= ~3;
	mem = getDmaRam(adr, &words_max);
	if (mem == INVALID_PTR || size > words_max) {
		log_unhandled("bad dma0 madr %x\n", adr);
		HW_DMA0_CHCR &= SWAP32(~0x01000000);
		return;
	}

	switch (cmd >> 28) {
		case 0x3: // decode 15/24bpp
			mdec.rl = mem;
			/* now the mdec is busy till all data are decoded */
			mdec.reg1 |= MDEC1_BUSY;
			/* detect the end of decoding */
			mdec.rl_end = mdec.rl + (size * 2);

			/* sanity check */
			if(mdec.rl_end <= mdec.rl)
				break;

			/* process the pending dma1 */
			if(mdec.pending_dma1.adr){
				psxDma1(mdec.pending_dma1.adr, mdec.pending_dma1.bcr, mdec.pending_dma1.chcr);
			}
			mdec.pending_dma1.adr = 0;
			return;
			

		case 0x4: // quantization table upload
			{
				const u8 *p = mem;
				// printf("uploading new quantization table\n");
				// printmatrixu8(p);
				// printmatrixu8(p + 64);
				iqtab_init(iq_y, p);
				iqtab_init(iq_uv, p + 64);
			}
			break;

		case 0x6: // cosine table
			// printf("mdec cosine table\n");
			break;

		default:
			log_unhandled("mdec: unknown command %08x\n", cmd);
			break;
	}

	set_event(PSXINT_MDECINDMA, size);
}

void mdec0Interrupt()
{
	if (HW_DMA0_CHCR & SWAP32(0x01000000))
	{
		HW_DMA0_CHCR &= SWAP32(~0x01000000);
		DMA_INTERRUPT(0);
	}
}

#define SIZE_OF_24B_BLOCK (16*16*3)
#define SIZE_OF_16B_BLOCK (16*16*2)

void psxDma1(u32 adr, u32 bcr, u32 chcr) {
	u32 words, words_max = 0;
	int blk[DSIZE2 * 6];
	u8 * image;
	int size;

	if (chcr != 0x01000200) {
		log_unhandled("mdec1: invalid dma %08x\n", chcr);
		return;
	}

	words = (bcr >> 16) * (bcr & 0xffff);
	/* size in byte */
	size = words * 4;

	if (!(mdec.reg1 & MDEC1_BUSY)) {
		/* add to pending */
		mdec.pending_dma1.adr = adr;
		mdec.pending_dma1.bcr = bcr;
		mdec.pending_dma1.chcr = chcr;
		/* do not free the dma */
		return;
	}

	adr &= ~3;
	image = getDmaRam(adr, &words_max);
	if (image == INVALID_PTR || words > words_max) {
		log_unhandled("bad dma1 madr %x\n", adr);
		HW_DMA1_CHCR &= SWAP32(~0x01000000);
		return;
	}

	if (mdec.reg0 & MDEC0_RGB24) {
		/* 16 bits decoding
		 * block are 16 px * 16 px, each px are 2 byte
		 */

		/* there is some partial block pending ? */
		if(mdec.block_buffer_pos != 0) {
			int n = mdec.block_buffer - mdec.block_buffer_pos + SIZE_OF_16B_BLOCK;
			/* TODO: check if partial block do not  larger than size */
			memcpy(image, mdec.block_buffer_pos, n);
			image += n;
			size -= n;
			mdec.block_buffer_pos = 0;
		}

		while(size >= SIZE_OF_16B_BLOCK) {
			mdec.rl = rl2blk(blk, mdec.rl, iq_y, iq_uv);
			yuv2rgb15(blk, (u16 *)image, STP_A, Config.Mdec);
			image += SIZE_OF_16B_BLOCK;
			size -= SIZE_OF_16B_BLOCK;
		}

		if(size != 0) {
			mdec.rl = rl2blk(blk, mdec.rl, iq_y, iq_uv);
			yuv2rgb15(blk, (u16 *)mdec.block_buffer, STP_A, Config.Mdec);
			memcpy(image, mdec.block_buffer, size);
			mdec.block_buffer_pos = mdec.block_buffer + size;
		}

	} else {
		/* 24 bits decoding
		 * block are 16 px * 16 px, each px are 3 byte
		 */

		/* there is some partial block pending ? */
		if(mdec.block_buffer_pos != 0) {
			int n = mdec.block_buffer - mdec.block_buffer_pos + SIZE_OF_24B_BLOCK;
			/* TODO: check if partial block do not  larger than size */
			memcpy(image, mdec.block_buffer_pos, n);
			image += n;
			size -= n;
			mdec.block_buffer_pos = 0;
		}

		while(size >= SIZE_OF_24B_BLOCK) {
			mdec.rl = rl2blk(blk, mdec.rl, iq_y, iq_uv);
			yuv2rgb24(blk, image, Config.Mdec);
			image += SIZE_OF_24B_BLOCK;
			size -= SIZE_OF_24B_BLOCK;
		}

		if(size != 0) {
			mdec.rl = rl2blk(blk, mdec.rl, iq_y, iq_uv);
			yuv2rgb24(blk, mdec.block_buffer, Config.Mdec);
			memcpy(image, mdec.block_buffer, size);
			mdec.block_buffer_pos = mdec.block_buffer + size;
		}
	}
	if (size < 0)
		log_unhandled("mdec: bork\n");
	
	/* define the power of mdec */
	set_event(PSXINT_MDECOUTDMA, words * MDEC_BIAS + MDEC_DELAY);
	/* some CPU stalling */
	psxRegs.cycle += words * MDEC_BIAS / 4;
}

void mdec1Interrupt() {
	/* Author : gschwind
	 *
	 * in that case we have done all decoding stuff
	 * Note that : each block end with 0xfe00 flags
	 * the list of blocks end with the same 0xfe00 flags
	 * data loock like :
	 *
	 *  data block ...
	 *  0xfe00
	 *  data block ...
	 *  0xfe00
	 *  a lost of block ..
	 *
	 *  0xfe00
	 *  the last block
	 *  0xfe00
	 *  0xfe00
	 *
	 * OR
	 *
	 * if the 0xfe00 is not present the data size is important.
	 *
	 */

	/* MDEC_END_OF_DATA avoids read outside memory */
	//printf("mdec left %zd, v=%04x\n", mdec.rl_end - mdec.rl, *(mdec.rl));
	if (mdec.rl >= mdec.rl_end || SWAP16(*(mdec.rl)) == MDEC_END_OF_DATA) {
		mdec.reg1 &= ~(MDEC1_STP|MDEC1_BUSY);
		if (HW_DMA0_CHCR & SWAP32(0x01000000))
		{
			HW_DMA0_CHCR &= SWAP32(~0x01000000);
			DMA_INTERRUPT(0);
		}
	}

	if (HW_DMA1_CHCR & SWAP32(0x01000000))
	{
		HW_DMA1_CHCR &= SWAP32(~0x01000000);
		DMA_INTERRUPT(1);
	}
}

int mdecFreeze(void *f, int Mode) {
	u8 *base = (u8 *)psxRegs.ptrs.psxM;
	u32 v;

	gzfreeze(&mdec.reg0, sizeof(mdec.reg0));
	gzfreeze(&mdec.reg1, sizeof(mdec.reg1));

	v = (u8 *)mdec.rl - base;
	gzfreeze(&v, sizeof(v));
	mdec.rl = (u16 *)(base + (v & 0x1ffffe));
	v = (u8 *)mdec.rl_end - base;
	gzfreeze(&v, sizeof(v));
	mdec.rl_end = (u16 *)(base + (v & 0x1ffffe));

	v = 0;
	if (mdec.block_buffer_pos)
		v = mdec.block_buffer_pos - mdec.block_buffer;
	gzfreeze(&v, sizeof(v));
	mdec.block_buffer_pos = 0;
	if (v && v < sizeof(mdec.block_buffer))
		mdec.block_buffer_pos = mdec.block_buffer;

	gzfreeze(&mdec.block_buffer, sizeof(mdec.block_buffer));
	gzfreeze(&mdec.pending_dma1, sizeof(mdec.pending_dma1));
	gzfreeze(iq_y, sizeof(iq_y));
	gzfreeze(iq_uv, sizeof(iq_uv));

	return 0;
}
