/*
 * ym_tl2_proof.c -- the tl2 lever, proven before it ships.
 *
 * tl2 replaces op_calc/op_calc1's
 *
 *     ret = ym_tl_tab[sin | (env<<7)];
 *
 * -- a 213 KB XIP table load executed once per operator-sample -- with
 *
 *     p  = (env<<2) + ym_sin_tab[sin];
 *     ret = (p >= 13*TL_RES_LEN) ? 0 : ym_tl_tab2[p & 0xff] >> (p>>8);
 *
 * which needs only ym_sin_tab (256 entries) and the first 256 entries of
 * ym_tl_tab2 -- 1 KB of RAM.  Why it is exact: init_tables' compose loop
 * writes ym_tl_tab[(y<<7)|x] with y even and x in [0,256); the two aliasing
 * writers of any address (row r, col c) are (y=r, x=c) and (y=r-(c>>7),
 * x=c|((c&0x80)^0x80))... concretely: x in [128,256) lands one row up, and
 * that row is odd for every address op_calc (even env) reads, so the sole
 * writer of (even row, any col) is (y=env, x=sin).  The written value is
 * then the row formula above, with ym_tl_tab2[p] = n[p & 0xff] >> (p>>8)
 * by the tl_tab2 fill (n at row 0, shifted copies below).
 *
 * This program proves it two ways:
 *   stage 1 (-b <file>): the ported fill below is byte-identical to the
 *       213,776 bytes of ym_tl_tab actually linked into the shipped image
 *       (objcopy --dump-section .rodata_md32x + dd at the symbol offset).
 *   stage 2 (always): the computed form matches the table for ALL 106,496
 *       (even env, sin) combinations op_calc can reach.
 *
 * Build: gcc -O2 -o ym_tl2_proof tools/ym_tl2_proof.c -lm
 * A rig gate complements this: the patched core must reproduce snd hash
 * a1f2c025 over 900 frames of the Doom demo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* verbatim from pico/sound/ym2612.c */
#define ENV_BITS	10
#define SIN_BITS	10
#define ENV_LEN		(1<<ENV_BITS)
#define SIN_LEN		(1<<SIN_BITS)
#define ENV_STEP	(128.0/ENV_LEN)
#define TL_RES_LEN	(256)
#define TL_TAB_LEN	(13*TL_RES_LEN*256/8) /* 106496 */

static unsigned short ref_sin[256];
static unsigned short ref_tl2[13*TL_RES_LEN];
static unsigned short ref_tl[TL_TAB_LEN];

int main(int argc, char **argv)
{
	signed int i, x, y, p, n;
	double o, m;
	long combos = 0, mism = 0;

	/* ---- verbatim port of init_tables() fill (ym2612.c) ---- */
	for (i = 0; i < 256; i++)
	{
		m = sin(((i*2)+1) * M_PI / SIN_LEN);
		if (m > 0.0) o = 8*log(1.0/m)/log(2);
		else         o = 8*log(-1.0/m)/log(2);
		o = o / (ENV_STEP/4);
		n = (int)(2.0*o);
		if (n&1) n = (n>>1)+1;
		else     n = n>>1;
		ref_sin[i] = n;
	}

	for (x = 0; x < TL_RES_LEN; x++)
	{
		m = (1<<16) / pow(2, (x+1) * (ENV_STEP/4.0) / 8.0);
		m = floor(m);
		n = (int)m;
		n >>= 4;
		if (n&1) n = (n>>1)+1;
		else     n = n>>1;
		n <<= 2;
		ref_tl2[x] = n;
		for (i = 1; i < 13; i++)
			ref_tl2[x + i*TL_RES_LEN] = n >> i;
	}

	for (x = 0; x < 256; x++)
	{
		int s = ref_sin[x];
		for (y = 0; y < 2*13*TL_RES_LEN/8; y += 2)
		{
			p = (y<<2) + s;
			if (p >= 13*TL_RES_LEN)
				ref_tl[(y<<7) | x] = 0;
			else
				ref_tl[(y<<7) | x] = ref_tl2[p];
		}
	}

	/* ---- stage 1: compare against the linked image ---- */
	if (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'b')
	{
		FILE *f = fopen(argv[2], "rb");
		long diff = 0;
		if (!f) { perror(argv[2]); return 2; }
		for (i = 0; i < TL_TAB_LEN; i++)
		{
			int c1 = fgetc(f), c2 = fgetc(f);
			unsigned short v;
			if (c1 == EOF || c2 == EOF) {
				fprintf(stderr, "short read at %d\n", i);
				return 2;
			}
			v = (unsigned short)(c1 | (c2<<8)); /* LSB_FIRST target */
			if (v != ref_tl[i]) { if (!diff) printf("first diff @ %d: img %u ref %u\n", i, v, ref_tl[i]); diff++; }
		}
		fclose(f);
		printf("stage1: image bytes=%d mismatches=%ld\n", TL_TAB_LEN, diff);
		if (diff) return 1;
	}

	/* ---- stage 2: computed form == table, all reachable lookups ---- */
	{
		unsigned env;
		int sin;
		for (env = 0; env < 2*13*TL_RES_LEN/8; env += 2) /* ENV_QUIET max, even only */
		{
			for (sin = 0; sin < 256; sin++)
			{
				int p_ = ((int)env << 2) + ref_sin[sin];
				unsigned short got = (p_ >= 13*TL_RES_LEN) ? 0 :
					(unsigned short)(ref_tl2[p_ & 0xff] >> (p_ >> 8));
				if (got != ref_tl[(env<<7) | sin])
				{
					if (!mism)
						printf("first diff @ env=%u sin=%d: got %u want %u\n",
							env, sin, got, ref_tl[(env<<7)|sin]);
					mism++;
				}
				combos++;
			}
		}
	}
	printf("stage2: combos=%ld mismatches=%ld\n", combos, mism);
	if (mism) return 1;

	puts("PASS");
	return 0;
}
