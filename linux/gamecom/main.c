// license:BSD-3-Clause
// Headless host harness for the Tiger Game.com core.
// Loads the internal BIOS (+ optional kernel ROM) and a cartridge, runs a fixed
// number of frames, then dumps gamecom_frame.ppm and a boot-signal trace.
//
//   ./build/gamecom <internal.bin> <kernel.bin|-> <cart.bin|-> [frames]
//
// Deterministic, no SDL — same philosophy as the zx harness.

#include "gamecom_core.h"
#include "sm8500.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, int *len_out)
{
	if (!path || strcmp(path, "-") == 0) { *len_out = 0; return NULL; }
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "ERR: cannot open %s\n", path); *len_out = 0; return NULL; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) { fclose(f); *len_out = 0; return NULL; }
	uint8_t *buf = (uint8_t *)malloc(n);
	if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); *len_out = 0; return NULL; }
	fclose(f);
	*len_out = (int)n;
	return buf;
}

static void dump_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "ERR: cannot write %s\n", path); return; }
	fprintf(f, "P6\n%d %d\n255\n", GAMECOM_W, GAMECOM_H);
	for (int i = 0; i < GAMECOM_W * GAMECOM_H; i++) {
		uint8_t idx = gamecom_fb[i];
		if (idx > 4) idx = 4;
		fwrite(gamecom_palette[idx], 1, 3, f);
	}
	fclose(f);
}

static int nonblank_pixels(void)
{
	int n = 0;
	for (int i = 0; i < GAMECOM_W * GAMECOM_H; i++)
		if (gamecom_fb[i] != 0) n++;
	return n;
}

int main(int argc, char **argv)
{
	const char *irom_path = argc > 1 ? argv[1] : "internal.bin";
	const char *krom_path = argc > 2 ? argv[2] : "external.bin";
	const char *cart_path = argc > 3 ? argv[3] : "-";
	int frames = argc > 4 ? atoi(argv[4]) : 600;
	/* optional scripted touchscreen tap:  tap_x tap_y tap_start [tap_len] */
	int tap_x     = argc > 5 ? atoi(argv[5]) : -1;
	int tap_y     = argc > 6 ? atoi(argv[6]) : -1;
	int tap_start = argc > 7 ? atoi(argv[7]) : 0;
	int tap_len   = argc > 8 ? atoi(argv[8]) : 20;

	int irom_len, krom_len, cart_len;
	uint8_t *irom = slurp(irom_path, &irom_len);
	uint8_t *krom = slurp(krom_path, &krom_len);
	uint8_t *cart = slurp(cart_path, &cart_len);

	printf("game.com host harness\n");
	printf("  internal BIOS : %s (%d bytes)\n", irom_path, irom_len);
	printf("  kernel ROM    : %s (%d bytes)\n", krom_path, krom_len);
	printf("  cartridge     : %s (%d bytes)\n", cart_path, cart_len);
	printf("  frames        : %d\n", frames);

	if (!irom || irom_len < 0x1000) {
		fprintf(stderr, "FATAL: internal BIOS (4KB internal.bin) is required.\n");
		return 1;
	}

	if (gamecom_init(irom, irom_len, krom, krom_len, cart, cart_len) != 0) {
		fprintf(stderr, "FATAL: gamecom_init failed.\n");
		return 1;
	}

	gamecom_set_input_state(0xFF, 0xFF, 0xFF);   /* nothing pressed */

	int lcd_on_frame = -1;
	int max_nonblank = 0;
	if (tap_x >= 0)
		printf("  scripted tap  : (%d,%d) frames %d..%d\n", tap_x, tap_y, tap_start, tap_start + tap_len - 1);

	/* test driver: pulse button A every GC_A_PERIOD frames after GC_A_FROM, to
	 * confirm menus and advance into gameplay (mirrors device A=button+centre tap) */
	int a_from   = getenv("GC_A_FROM")   ? atoi(getenv("GC_A_FROM"))   : -1;
	int a_period = getenv("GC_A_PERIOD") ? atoi(getenv("GC_A_PERIOD")) : 90;

	for (int fr = 0; fr < frames; fr++) {
		if (tap_x >= 0) {
			int down = (fr >= tap_start && fr < tap_start + tap_len);
			gamecom_set_stylus(tap_x, tap_y, down);
		}
		if (a_from >= 0 && fr >= a_from) {
			int phase = (fr - a_from) % a_period;
			int a_down = (phase < 8);   /* 8-frame A press, then release */
			gamecom_set_input_state(a_down ? (uint8_t)~GC_IN0_A : 0xFF, 0xFF, 0xFF);
			gamecom_set_stylus(GAMECOM_W/2, GAMECOM_H/2, a_down);
		}
		gamecom_run_frame();

		int nb = nonblank_pixels();
		if (nb > max_nonblank) max_nonblank = nb;
		if (lcd_on_frame < 0 && nb > 0) lcd_on_frame = fr;

		if (fr % 60 == 0 || fr == frames - 1)
			printf("  frame %4d: PC=%04X  nonblank=%d\n", fr, sm8500_pc(), nb);

		if (getenv("GC_DUMP_EVERY")) {
			int every = atoi(getenv("GC_DUMP_EVERY"));
			if (every > 0 && (fr % every == 0)) {
				char nm[64];
				snprintf(nm, sizeof nm, "/tmp/gc_f%05d.ppm", fr);
				dump_ppm(nm);
			}
		}
	}

	dump_ppm("gamecom_frame.ppm");
	printf("--- summary ---\n");
	printf("  first non-blank frame : %d\n", lcd_on_frame);
	printf("  peak non-blank pixels : %d / %d\n", max_nonblank, GAMECOM_W * GAMECOM_H);
	printf("  final PC              : %04X\n", sm8500_pc());
	printf("  wrote gamecom_frame.ppm (%dx%d)\n", GAMECOM_W, GAMECOM_H);

	free(irom); free(krom); free(cart);
	return 0;
}
