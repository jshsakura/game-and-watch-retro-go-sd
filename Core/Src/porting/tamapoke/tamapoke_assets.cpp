/* One-file asset container on the card.
 *
 * Upstream keeps every sprite pack as its own file under /mons. That works on a
 * board with an SD library and no other users of the filesystem; here it means
 * three hundred entries on the card and a file handle taken every time the pet
 * changes species, against a FatFS layer the whole firmware shares.
 *
 * So the packs, the thumbnails and the names ride in a single
 * /roms/homebrew/tamapoke_assets.dat. That is also exactly the shape Game & What
 * already carries for homebrew -- a .bin payload plus a .dat sidecar, the way
 * Super Mario World and Zelda 3 get their data -- so that project needs no
 * changes to build a card with this on it.
 *
 * Container is upstream's own TPAK (tools/tamapoke/pack_assets_dat.py):
 *   "TPAK", u16 count, count x { u8 nameLen, name, u32 size }, then the blobs.
 */
#include "tamapoke_assets.h"

#include <stdio.h>
#include <string.h>

#define ASSETS_PATH "/roms/homebrew/tamapoke_assets.dat"
#define TPAK_MAGIC "TPAK"
#define TPAK_MAX_ENTRIES 320
#define TPAK_NAME_MAX 16

typedef struct {
  char name[TPAK_NAME_MAX];
  uint32_t offset;
  uint32_t size;
} entry_t;

/* The index only: about 8 KB, held so a lookup costs no I/O. The blobs stay on
 * the card and are read on demand into the caller's buffer. */
static entry_t g_index[TPAK_MAX_ENTRIES];
static uint16_t g_count;
static bool g_ready;

bool tamapoke_assets_open(void) {
  if (g_ready) return true;

  FILE *f = fopen(ASSETS_PATH, "rb");
  if (!f) return false;

  char magic[4];
  uint16_t count;
  if (fread(magic, 1, 4, f) != 4 || memcmp(magic, TPAK_MAGIC, 4) != 0 ||
      fread(&count, 1, 2, f) != 2) {
    fclose(f);
    return false;
  }
  if (count > TPAK_MAX_ENTRIES) count = TPAK_MAX_ENTRIES;

  /* Walk the index, accumulating offsets. Blob N starts where the index ends
   * plus the sizes of every blob before it, so the file itself never has to
   * store an absolute offset that a repack could invalidate. */
  uint32_t cursor = 0;
  for (uint16_t i = 0; i < count; i++) {
    uint8_t name_len;
    if (fread(&name_len, 1, 1, f) != 1) break;

    char name[256];
    if (fread(name, 1, name_len, f) != name_len) break;
    name[name_len] = '\0';

    uint32_t size;
    if (fread(&size, 1, 4, f) != 4) break;

    if (name_len < TPAK_NAME_MAX) {
      memcpy(g_index[g_count].name, name, name_len + 1);
      g_index[g_count].offset = cursor;
      g_index[g_count].size = size;
      g_count++;
    }
    cursor += size;
  }

  /* Offsets so far are relative to the first blob; make them absolute now that
   * the index length is known. */
  long data_start = ftell(f);
  fclose(f);
  if (data_start < 0 || g_count == 0) {
    g_count = 0;
    return false;
  }
  for (uint16_t i = 0; i < g_count; i++) g_index[i].offset += (uint32_t)data_start;

  g_ready = true;
  return true;
}

uint32_t tamapoke_assets_read(const char *name, uint8_t *dst, uint32_t cap) {
  if (!g_ready || !name || !dst) return 0;

  const entry_t *e = NULL;
  for (uint16_t i = 0; i < g_count; i++) {
    if (strcmp(g_index[i].name, name) == 0) {
      e = &g_index[i];
      break;
    }
  }
  /* A pack larger than the caller's buffer means the asset pipeline emitted
   * full-size sprites; growing at runtime is not an option here, so refuse. */
  if (!e || e->size == 0 || e->size > cap) return 0;

  FILE *f = fopen(ASSETS_PATH, "rb");
  if (!f) return 0;
  if (fseek(f, (long)e->offset, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }
  size_t got = fread(dst, 1, e->size, f);
  fclose(f);
  return got == e->size ? e->size : 0;
}

bool tamapoke_assets_has(const char *name) {
  if (!g_ready || !name) return false;
  for (uint16_t i = 0; i < g_count; i++)
    if (strcmp(g_index[i].name, name) == 0) return true;
  return false;
}
