// Repro driver: links the REAL device cover decoders (music_cover.c + tjpgd.c +
// progjpeg.c + music_lupng.c + miniz.c) and renders a sidecar cover into a
// 320x240 RGB565 framebuffer, dumped to a .bin for check.py to inspect.
//
//   ./driver <track.mp3 path> <out.bin>
//
// id3_locate_cover is stubbed to always-miss so cover_load() takes the sidecar
// path (we don't link music_id3.c).
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "music_cover.h"

static uint16_t g_fb[320 * 240];

void *lcd_get_active_buffer(void) { return g_fb; }
void *lcd_clear_active_buffer(void) { memset(g_fb, 0, sizeof g_fb); return g_fb; }
void lcd_swap(void) {}

bool id3_locate_cover(const char *path, long *off, long *len, bool *is_png) {
    (void)path; (void)off; (void)len; (void)is_png;
    return false;   // force sidecar lookup
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <track> <out.bin>\n", argv[0]); return 1; }
    memset(g_fb, 0, sizeof g_fb);

    bool is_png = false;
    int n = cover_load(argv[1], &is_png);
    if (n <= 0) { fprintf(stderr, "FAIL no cover: %s\n", argv[1]); return 2; }

    bool ok = cover_render_card(n, is_png, 0, 0, 320, 240);
    if (!ok) { fprintf(stderr, "FAIL render (is_png=%d): %s\n", is_png, argv[1]); return 3; }

    FILE *f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "FAIL open out: %s\n", argv[2]); return 4; }
    fwrite(g_fb, sizeof(uint16_t), 320 * 240, f);
    fclose(f);
    fprintf(stderr, "OK is_png=%d n=%d -> %s\n", is_png, n, argv[2]);
    return 0;
}
