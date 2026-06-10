// Persistent SD thumbnail cache — see media_cache.h.

#include "media_cache.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>     // mkdir (wraps f_mkdir, see syscalls.c)

#define CACHE_MAGIC   0x3143544Du   // "MTC1" little-endian
#define MAX_THUMB_SZ  140           // guards the per-read pixel buffer / cap

// 12-byte header prefixing the RGB565 pixels in each cache file.
typedef struct {
    uint32_t magic;
    uint16_t sz;            // thumbnail dimension (sz×sz)
    uint16_t reserved;
    uint32_t src_size;      // source track byte size, for invalidation
} cache_hdr_t;

static char g_dir[256];     // "<root>/.mthumb"  (empty => cache disabled)

// FNV-1a 32-bit hash of the track path — the cache filename stem.
static uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static bool cache_file(const char *path, int sz, char *out, int outsz)
{
    if (!g_dir[0]) return false;
    snprintf(out, outsz, "%s/%08lx_%d.mtc", g_dir,
             (unsigned long)path_hash(path), sz);
    return true;
}

void cache_init(const char *music_root)
{
    g_dir[0] = '\0';
    if (!music_root || !music_root[0]) return;
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/.mthumb", music_root);
    mkdir(dir, 0777);                       // ok if it already exists
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
}

bool cache_get(const char *path, long src_size, uint16_t *out, int sz)
{
    char cf[300];
    if (sz <= 0 || sz > MAX_THUMB_SZ || !cache_file(path, sz, cf, sizeof(cf)))
        return false;

    FILE *f = fopen(cf, "rb");
    if (!f) return false;

    cache_hdr_t h;
    bool ok = false;
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) &&
        h.magic == CACHE_MAGIC && h.sz == (uint16_t)sz &&
        h.src_size == (uint32_t)src_size) {
        size_t n = (size_t)sz * sz;
        ok = (fread(out, sizeof(uint16_t), n, f) == n);
    }
    fclose(f);
    return ok;
}

void cache_put(const char *path, long src_size, const uint16_t *thumb, int sz)
{
    char cf[300];
    if (sz <= 0 || sz > MAX_THUMB_SZ || !cache_file(path, sz, cf, sizeof(cf)))
        return;

    FILE *f = fopen(cf, "wb");
    if (!f) return;

    cache_hdr_t h = { CACHE_MAGIC, (uint16_t)sz, 0, (uint32_t)src_size };
    if (fwrite(&h, 1, sizeof(h), f) == sizeof(h))
        fwrite(thumb, sizeof(uint16_t), (size_t)sz * sz, f);
    fclose(f);
}
