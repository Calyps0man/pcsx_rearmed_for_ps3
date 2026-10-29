/***************************************************************************
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

#ifndef __MDEC_H__
#define __MDEC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "psxcommon.h"
#include "r3000a.h"
#include "psxhw.h"
#include "psxdma.h"

void mdecInit();
void mdecWrite0(u32 data);
void mdecWrite1(u32 data);
u32 mdecRead0();
u32 mdecRead1();
void psxDma0(u32 madr, u32 bcr, u32 chcr);
extern unsigned int mdec_dma1_count;   /* MDEC output DMAs so far (FMV detection) */
void psxDma1(u32 madr, u32 bcr, u32 chcr);
void mdec0Interrupt();
void mdec1Interrupt();
int mdecFreeze(void *f, int Mode);

/* Optional: a front end can decode whole macroblocks elsewhere (the PS3 SPUs)
 * with the routines in mdec_decode.h. Decodes up to n macroblocks from *rl into
 * image (16 bpp: 512 bytes each, 24 bpp: 768), advances *rl and returns how
 * many it decoded; 0 leaves them to the core. stp: 0x8000 or 0, bw: monochrome. */
extern int (*mdec_decode_hook)(const unsigned short **rl, unsigned char *image, int n,
		int rgb24, int stp, int bw, const int *iq_y, const int *iq_uv);
/* Optional: told when decoding of a run-length stream starts (rl..rl_end, the
 * game leaves that RAM alone until the MDEC has consumed it), so the front end
 * can decode ahead; rl NULL: forget any stream (reset, savestate load). */
extern void (*mdec_stream_hook)(const unsigned short *rl, const unsigned short *rl_end,
		int rgb24, int stp, int bw, const int *iq_y, const int *iq_uv);

#ifdef __cplusplus
}
#endif
#endif
