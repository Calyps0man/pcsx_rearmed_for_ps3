/* MDEC macroblock decoding (run-length/IDCT/colour conversion), shared by
 * mdec.c and by front ends that decode on other processors (the PS3 SPUs), so
 * both produce identical output. Self-contained: the includer defines
 * MDEC_MDEC_SWAP16(x) to convert a little-endian PS1 halfword to/from host order. */
#ifndef __MDEC_DECODE_H__
#define __MDEC_DECODE_H__

#define DSIZE			8
#define DSIZE2			(DSIZE * DSIZE)

#define SCALE(x, n)		((x) >> (n))
#define SCALER(x, n)	(((x) + ((1 << (n)) >> 1)) >> (n))

#define AAN_CONST_BITS			12
#define AAN_PRESCALE_BITS		16

#define AAN_CONST_SIZE			24
#define AAN_CONST_SCALE			(AAN_CONST_SIZE - AAN_CONST_BITS)

#define AAN_PRESCALE_SIZE		20
#define AAN_PRESCALE_SCALE		(AAN_PRESCALE_SIZE-AAN_PRESCALE_BITS)
#define AAN_EXTRA				12

#define FIX_1_082392200		SCALER(18159528, AAN_CONST_SCALE) // B6
#define FIX_1_414213562		SCALER(23726566, AAN_CONST_SCALE) // A4
#define FIX_1_847759065		SCALER(31000253, AAN_CONST_SCALE) // A2
#define FIX_2_613125930		SCALER(43840978, AAN_CONST_SCALE) // B2

#define MULS(var, const)	(SCALE((var) * (const), AAN_CONST_BITS))

#define	RLE_RUN(a)	((a) >> 10)
#define	RLE_VAL(a)	(((int)(a) << (sizeof(int) * 8 - 10)) >> (sizeof(int) * 8 - 10))

static inline void fillcol(int *blk, int val) {
	blk[0 * DSIZE] = blk[1 * DSIZE] = blk[2 * DSIZE] = blk[3 * DSIZE]
		= blk[4 * DSIZE] = blk[5 * DSIZE] = blk[6 * DSIZE] = blk[7 * DSIZE] = val;
}

static inline void fillrow(int *blk, int val) {
	blk[0] = blk[1] = blk[2] = blk[3]
		= blk[4] = blk[5] = blk[6] = blk[7] = val;
}

static void idct(int *block,int used_col) {
	int tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
	int z5, z10, z11, z12, z13;
	int *ptr;
	int i;

	// the block has only the DC coefficient
	if (used_col == -1) { 
		int v = block[0];
		for (i = 0; i < DSIZE2; i++) block[i] = v;
		return;
	}

	// last_col keeps track of the highest column with non zero coefficients
	ptr = block;
	for (i = 0; i < DSIZE; i++, ptr++) {
		if ((used_col & (1 << i)) == 0) {
			// the column is empty or has only the DC coefficient
			if (ptr[DSIZE * 0]) {
				fillcol(ptr, ptr[0]);
				used_col |= (1 << i);
			}
			continue;
		}

		// further optimization could be made by keeping track of 
		// last_row in rl2blk
		z10 = ptr[DSIZE * 0] + ptr[DSIZE * 4]; // s04
		z11 = ptr[DSIZE * 0] - ptr[DSIZE * 4]; // d04
		z13 = ptr[DSIZE * 2] + ptr[DSIZE * 6]; // s26
		z12 = MULS(ptr[DSIZE * 2] - ptr[DSIZE * 6], FIX_1_414213562) - z13; 
		//^^^^  d26=d26*2*A4-s26

		tmp0 = z10 + z13; // os07 = s04 + s26
		tmp3 = z10 - z13; // os34 = s04 - s26
		tmp1 = z11 + z12; // os16 = d04 + d26
		tmp2 = z11 - z12; // os25 = d04 - d26

		z13 = ptr[DSIZE * 3] + ptr[DSIZE * 5]; //s53
		z10 = ptr[DSIZE * 3] - ptr[DSIZE * 5]; //-d53 
		z11 = ptr[DSIZE * 1] + ptr[DSIZE * 7]; //s17
		z12 = ptr[DSIZE * 1] - ptr[DSIZE * 7]; //d17

		tmp7 = z11 + z13; // od07 = s17 + s53

		z5 = (z12 - z10) * (FIX_1_847759065); 
		tmp6 = SCALE(z10*(FIX_2_613125930) + z5, AAN_CONST_BITS) - tmp7; 
		tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6;
		tmp4 = SCALE(z12*(FIX_1_082392200) - z5, AAN_CONST_BITS) + tmp5; 

		// path #1
		//z5 = (z12 - z10)* FIX_1_847759065; 
		// tmp0 = (d17 + d53) * 2*A2

		//tmp6 = DESCALE(z10*FIX_2_613125930 + z5, CONST_BITS) - tmp7; 
		// od16 = (d53*-2*B2 + tmp0) - od07

		//tmp4 = DESCALE(z12*FIX_1_082392200 - z5, CONST_BITS) + tmp5; 
		// od34 = (d17*2*B6 - tmp0) + od25

		// path #2

		// od34 = d17*2*(B6-A2) - d53*2*A2
		// od16 = d53*2*(A2-B2) + d17*2*A2

		// end

		//    tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6;
		// od25 = (s17 - s53)*2*A4 - od16

		ptr[DSIZE * 0] = (tmp0 + tmp7); // os07 + od07
		ptr[DSIZE * 7] = (tmp0 - tmp7); // os07 - od07
		ptr[DSIZE * 1] = (tmp1 + tmp6); // os16 + od16
		ptr[DSIZE * 6] = (tmp1 - tmp6); // os16 - od16
		ptr[DSIZE * 2] = (tmp2 + tmp5); // os25 + od25
		ptr[DSIZE * 5] = (tmp2 - tmp5); // os25 - od25
		ptr[DSIZE * 4] = (tmp3 + tmp4); // os34 + od34
		ptr[DSIZE * 3] = (tmp3 - tmp4); // os34 - od34
	}

	ptr = block;
	if (used_col == 1) {
		for (i = 0; i < DSIZE; i++)
			fillrow(block + DSIZE * i, block[DSIZE * i]);    
	} else {
		for (i = 0; i < DSIZE; i++, ptr += DSIZE) {
			z10 = ptr[0] + ptr[4];
			z11 = ptr[0] - ptr[4];
			z13 = ptr[2] + ptr[6];
			z12 = MULS(ptr[2] - ptr[6], FIX_1_414213562) - z13;

			tmp0 = z10 + z13;
			tmp3 = z10 - z13;
			tmp1 = z11 + z12;
			tmp2 = z11 - z12;
			
			z13 = ptr[3] + ptr[5];
			z10 = ptr[3] - ptr[5];
			z11 = ptr[1] + ptr[7];
			z12 = ptr[1] - ptr[7];

			tmp7 = z11 + z13;
			z5 = (z12 - z10) * FIX_1_847759065; 
			tmp6 = SCALE(z10 * FIX_2_613125930 + z5, AAN_CONST_BITS) - tmp7;
			tmp5 = MULS(z11 - z13, FIX_1_414213562) - tmp6;
			tmp4 = SCALE(z12 * FIX_1_082392200 - z5, AAN_CONST_BITS) + tmp5;

			ptr[0] = tmp0 + tmp7;

			ptr[7] = tmp0 - tmp7;
			ptr[1] = tmp1 + tmp6;
			ptr[6] = tmp1 - tmp6;
			ptr[2] = tmp2 + tmp5;
			ptr[5] = tmp2 - tmp5;
			ptr[4] = tmp3 + tmp4;
			ptr[3] = tmp3 - tmp4;
		}
	}
}

static const int zscan[DSIZE2] = {
	0 , 1 , 8 , 16, 9 , 2 , 3 , 10,
	17, 24, 32, 25, 18, 11, 4 , 5 ,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6 , 7 , 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

#define	MDEC_END_OF_DATA	0xfe00

static const unsigned short *rl2blk(int *blk, const unsigned short *mdec_rl,
		const int *iq_y, const int *iq_uv) {
	int i, k, q_scale, rl, used_col;
 	const int *iqtab;

	for (i = 0; i < 6 * DSIZE2; i++) blk[i] = 0;
	iqtab = iq_uv;
	for (i = 0; i < 6; i++) {
		// decode blocks (Cr,Cb,Y1,Y2,Y3,Y4)
		if (i == 2) iqtab = iq_y;

		rl = MDEC_SWAP16(*mdec_rl); mdec_rl++;
		q_scale = RLE_RUN(rl);
		blk[0] = SCALER(iqtab[0] * RLE_VAL(rl), AAN_EXTRA - 3);
		for (k = 0, used_col = 0;;) {
			rl = MDEC_SWAP16(*mdec_rl); mdec_rl++;
			if (rl == MDEC_END_OF_DATA) break;
			k += RLE_RUN(rl) + 1;	// skip zero-coefficients

			if (k > 63) {
				// printf("run lenght exceeded 64 enties\n");
				break;
			}

			// zigzag transformation
			blk[zscan[k]] = SCALER(RLE_VAL(rl) * iqtab[k] * q_scale, AAN_EXTRA);
			// keep track of used columns to speed up the idtc
			used_col |= (zscan[k] > 7) ? 1 << (zscan[k] & 7) : 0;
		}

		if (k == 0) used_col = -1;
		// used_col is -1 for blocks with only the DC coefficient
		// any other value is a bitmask of the columns that have 
		// at least one non zero cofficient in the rows 1-7
		// single coefficients in row 0 are treted specially 
		// in the idtc function
		idct(blk, used_col);
		blk += DSIZE2;
	}
	return mdec_rl;
}

// full scale (JPEG)
// Y/Cb/Cr[0...255] -> R/G/B[0...255]
// R = 1.000 * (Y) + 1.400 * (Cr - 128)
// G = 1.000 * (Y) - 0.343 * (Cb - 128) - 0.711 (Cr - 128)
// B = 1.000 * (Y) + 1.765 * (Cb - 128)
#define	MULR(a)			((1434 * (a))) 
#define	MULB(a)			((1807 * (a))) 
#define	MULG2(a, b)		((-351 * (a) - 728 * (b)))
#define MULY(a)			((a) << 10)

#define	MAKERGB15(r, g, b, a)	(MDEC_SWAP16(a | ((b) << 10) | ((g) << 5) | (r)))
#define	SCALE8(c)				SCALER(c, 20) 
#define SCALE5(c)				SCALER(c, 23)

static inline int clamp5(int v)
{
	v += 16;
	v = v < 0 ? 0 : (v > 31 ? 31 : v);
	return v;
}

static inline int clamp8(int v)
{
	v += 128;
	v = v < 0 ? 0 : (v > 255 ? 255 : v);
	return v;
}

#define CLAMP_SCALE8(a)   (clamp8(SCALE8(a)))
#define CLAMP_SCALE5(a)   (clamp5(SCALE5(a)))

static inline void putlinebw15(unsigned short *image, int *Yblk, int A) {
	int i;

	for (i = 0; i < 8; i++, Yblk++) {
		int Y = *Yblk;
		// missing rounding
		image[i] = MDEC_SWAP16((clamp5(Y >> 3) * 0x421) | A);
	}
}

static inline void putquadrgb15(unsigned short *image, int *Yblk, int Cr, int Cb, int A) {
	int Y, R, G, B;
	R = MULR(Cr);
	G = MULG2(Cb, Cr);
	B = MULB(Cb);

	// added transparency
	Y = MULY(Yblk[0]);
	image[0] = MAKERGB15(CLAMP_SCALE5(Y + R), CLAMP_SCALE5(Y + G), CLAMP_SCALE5(Y + B), A);
	Y = MULY(Yblk[1]);
	image[1] = MAKERGB15(CLAMP_SCALE5(Y + R), CLAMP_SCALE5(Y + G), CLAMP_SCALE5(Y + B), A);
	Y = MULY(Yblk[8]);
	image[16] = MAKERGB15(CLAMP_SCALE5(Y + R), CLAMP_SCALE5(Y + G), CLAMP_SCALE5(Y + B), A);
	Y = MULY(Yblk[9]);
	image[17] = MAKERGB15(CLAMP_SCALE5(Y + R), CLAMP_SCALE5(Y + G), CLAMP_SCALE5(Y + B), A);
}

/* A: 0x8000 to set the STP bit; bw: monochrome output */
static inline void yuv2rgb15(int *blk, unsigned short *image, int A, int bw) {
	int x, y;
	int *Yblk = blk + DSIZE2 * 2;
	int *Crblk = blk;
	int *Cbblk = blk + DSIZE2;

	if (!bw) {
		for (y = 0; y < 16; y += 2, Crblk += 4, Cbblk += 4, Yblk += 8, image += 24) {
			if (y == 8) Yblk += DSIZE2;
			for (x = 0; x < 4; x++, image += 2, Crblk++, Cbblk++, Yblk += 2) {
				putquadrgb15(image, Yblk, *Crblk, *Cbblk, A);
				putquadrgb15(image + 8, Yblk + DSIZE2, *(Crblk + 4), *(Cbblk + 4), A);
			}
		} 
	} else {
		for (y = 0; y < 16; y++, Yblk += 8, image += 16) {
			if (y == 8) Yblk += DSIZE2;
			putlinebw15(image, Yblk, A);
			putlinebw15(image + 8, Yblk + DSIZE2, A);
		}
	}
}

static inline void putlinebw24(unsigned char * image, int *Yblk) {
	int i;
	unsigned char Y;
	for (i = 0; i < 8 * 3; i += 3, Yblk++) {
		Y = clamp8(*Yblk);
		image[i + 0] = Y;
		image[i + 1] = Y;
		image[i + 2] = Y;
	}
}

static inline void putquadrgb24(unsigned char * image, int *Yblk, int Cr, int Cb) {
	int Y, R, G, B;

	R = MULR(Cr);
	G = MULG2(Cb,Cr);
	B = MULB(Cb);

	Y = MULY(Yblk[0]);
	image[0 * 3 + 0] = CLAMP_SCALE8(Y + R);
	image[0 * 3 + 1] = CLAMP_SCALE8(Y + G);
	image[0 * 3 + 2] = CLAMP_SCALE8(Y + B);
	Y = MULY(Yblk[1]);
	image[1 * 3 + 0] = CLAMP_SCALE8(Y + R);
	image[1 * 3 + 1] = CLAMP_SCALE8(Y + G);
	image[1 * 3 + 2] = CLAMP_SCALE8(Y + B);
	Y = MULY(Yblk[8]);
	image[16 * 3 + 0] = CLAMP_SCALE8(Y + R);
	image[16 * 3 + 1] = CLAMP_SCALE8(Y + G);
	image[16 * 3 + 2] = CLAMP_SCALE8(Y + B);
	Y = MULY(Yblk[9]);
	image[17 * 3 + 0] = CLAMP_SCALE8(Y + R);
	image[17 * 3 + 1] = CLAMP_SCALE8(Y + G);
	image[17 * 3 + 2] = CLAMP_SCALE8(Y + B);
}

static void yuv2rgb24(int *blk, unsigned char *image, int bw) {
	int x, y;
	int *Yblk = blk + DSIZE2 * 2;
	int *Crblk = blk;
	int *Cbblk = blk + DSIZE2;

	if (!bw) {
		for (y = 0; y < 16; y += 2, Crblk += 4, Cbblk += 4, Yblk += 8, image += 8 * 3 * 3) {
			if (y == 8) Yblk += DSIZE2;
			for (x = 0; x < 4; x++, image += 6, Crblk++, Cbblk++, Yblk += 2) {
				putquadrgb24(image, Yblk, *Crblk, *Cbblk);
				putquadrgb24(image + 8 * 3, Yblk + DSIZE2, *(Crblk + 4), *(Cbblk + 4));
			}
		}
	} else {
		for (y = 0; y < 16; y++, Yblk += 8, image += 16 * 3) {
			if (y == 8) Yblk += DSIZE2;
			putlinebw24(image, Yblk);
			putlinebw24(image + 8 * 3, Yblk + DSIZE2);
		}
	}
}

#endif
