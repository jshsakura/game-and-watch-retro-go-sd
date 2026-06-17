// Album-art rendering for the Music app.
//
// Decodes cover art (embedded ID3 APIC first, then a sidecar jpg/png beside the
// track) using TJpgDec (baseline JPEG) and lupng (PNG). The compressed image is
// STREAMED straight from the file during decode, so there is no raw-size cap and
// arbitrarily large covers work; only the PNG-inflate / JPEG-work scratch is
// owned here. (Progressive JPEG is unsupported by TJpgDec → placeholder shown.)
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Locate the track's cover and remember it as the active stream source (embedded
// APIC, else a sidecar). Returns a positive value (the image byte length, for a
// truthy "has cover") or 0 if none; sets *is_png (else treat as JPEG). The image
// is decoded on demand by the cover_render_*/cover_thumb calls below.
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

// Shared 352KB scratch (PNG inflate / JPEG work area). Exposed so the Video
// app's frame decoder can reuse it — Music and Video share the overlay and
// never run simultaneously, so it is free during video playback.
extern uint8_t g_scratch[];
