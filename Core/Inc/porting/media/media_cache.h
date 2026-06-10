// Persistent SD thumbnail cache for the Music browser.
//
// Decoding a cover thumbnail (JPEG/PNG) is expensive; doing it on every list
// repaint makes scrolling janky and re-visits slow. This caches each decoded
// sz×sz RGB565 thumbnail to a small file under "<root>/.mthumb/", keyed by the
// track path (hashed) and invalidated by the source file size. A cache hit is a
// ~2 KB read with no decode, so the list fills in once and is instant forever
// after. Cache files are tiny (sz*sz*2 + header) and hidden.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Point the cache at "<root>/.mthumb" and create the directory. Call once when
// the music root is known. If creation fails the cache silently disables.
void cache_init(const char *music_root);

// Fast path: if a valid thumbnail for (path, src_size, sz) is cached, copy its
// sz*sz RGB565 pixels into out and return true. No decode. Returns false on miss.
bool cache_get(const char *path, long src_size, uint16_t *out, int sz);

// Store a freshly decoded sz*sz RGB565 thumbnail for (path, src_size). Best
// effort — failures (full card, no dir) are ignored.
void cache_put(const char *path, long src_size, const uint16_t *thumb, int sz);
