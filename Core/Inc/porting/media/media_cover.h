// Album-art rendering for the Music app.
//
// Decodes cover art (embedded ID3 APIC first, then a sidecar jpg/png beside the
// track) using TJpgDec (scaled, any-size JPEG in ~8 KB) and lupng (PNG). Owns
// the shared image + decode scratch buffers.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Read the track's cover bytes into the internal image buffer. Returns the byte
// count (0 if no cover found) and sets *is_png (else treat as JPEG).
int  cover_load(const char *path, bool *is_png);

// Render the loaded cover (n bytes) fit to the full screen, centered, then
// darken the whole active framebuffer to make a dim backdrop. No swap.
bool cover_render_backdrop(int n, bool is_png);

// Render the loaded cover (n bytes) fit into the box (bx,by,bw,bh), centered.
// No clear, no swap — draws only the image pixels.
bool cover_render_card(int n, bool is_png, int bx, int by, int bw, int bh);

// Decode the track's cover into an sz×sz RGB565 thumbnail (JPEG only; PNG
// thumbnails are skipped). Returns true on success.
bool cover_thumb(const char *path, uint16_t *out, int sz);
