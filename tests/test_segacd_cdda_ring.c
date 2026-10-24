/* CD-DA prefetch ring in main_segacd.c: byte-exact against the unpatched read path.
 *
 * PR #66 first shipped a ring whose own reads re-entered the picodrive pm_read
 * hook (pm_read_audio ends in pm_read on little-endian), so every prefetch with
 * data already in the ring dropped that data and left the fd ahead of it: the
 * audio skipped thousands of bytes per frame. A test that models the ring but
 * not the hook cannot see that, so this one compiles the REAL ring lines out of
 * main_segacd.c (between the markers below) against a fake pm_file whose
 * pm_read / pm_seek call the same hook the picodrive patch adds.
 *
 * Reference: the unpatched sequence (audio reads and data-path reads/seeks on
 * ONE shared fd, no ring). Ring: the same operations with random prefetch calls
 * mixed in. Every returned byte and length must match.
 *
 * usage: test_segacd_cdda_ring <path to main_segacd.c> */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
typedef enum { PMT_UNCOMPRESSED = 0, PMT_CSO, PMT_CHD } pm_type;
typedef struct { void *file; void *param; int x; pm_type type; } pm_file;
typedef struct { pm_file *cdda_stream; } mcd_t;
static mcd_t mcd_obj, *Pico_mcd = &mcd_obj;

#define FILE_SIZE (2352 * 37 + 1111)
typedef struct { uint8_t data[FILE_SIZE]; long pos; } fake_t;

static int g_hooks_on;
void gnw_cdda_stream_touched(pm_file *stream);
void gnw_cdda_set_stream(pm_file *stream);
int gnw_cdda_read(void *buf, int bytes, pm_file *stream);

/* The picodrive patch calls the hook at the top of pm_read and pm_seek. */
static size_t pm_read(void *ptr, size_t bytes, pm_file *stream)
{
  if (g_hooks_on) gnw_cdda_stream_touched(stream);
  fake_t *f = stream->file;
  long left = FILE_SIZE - f->pos;
  size_t n = (long)bytes < left ? bytes : (size_t)(left > 0 ? left : 0);
  memcpy(ptr, f->data + f->pos, n);
  f->pos += (long)n;
  return n;
}
static size_t pm_read_audio(void *ptr, size_t bytes, pm_file *stream)
{
  return pm_read(ptr, bytes, stream); /* CPU_IS_LE build */
}
static int pm_seek(pm_file *stream, long off, int whence)
{
  if (g_hooks_on) gnw_cdda_stream_touched(stream);
  fake_t *f = stream->file;
  f->pos = whence == SEEK_CUR ? f->pos + off : off;
  if (f->pos < 0) f->pos = 0;
  return (int)f->pos;
}
static void cpumon_sleep(void) {}

/* ---- the real ring, cut out of main_segacd.c ---- */
#include "ring_extract.inc"

static uint32_t rng_state;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void fill(fake_t *f, uint32_t seed)
{
  uint32_t s = seed * 2654435761u + 1;
  for (int i = 0; i < FILE_SIZE; i++) { s = s * 1103515245u + 12345u; f->data[i] = (uint8_t)(s >> 16); }
  f->pos = 0;
}

int main(int argc, char **argv)
{
  (void)argc; (void)argv;
  int failures = 0, ops_total = 0, prefetch_total = 0, foreign_total = 0;
  for (uint32_t seed = 1; seed <= 400; seed++) {
    static fake_t ref, rg;
    pm_file pref = { &ref, 0, 0, PMT_UNCOMPRESSED }, prg = { &rg, 0, 0, PMT_UNCOMPRESSED };
    fill(&ref, seed); fill(&rg, seed);
    long start = (long)(rnd() % 4000);
    ref.pos = rg.pos = start;
    Pico_mcd->cdda_stream = &prg;
    g_hooks_on = 1;
    gnw_cdda_set_stream(&prg);
    rng_state = seed * 7919u;
    for (int op = 0; op < 500; op++) {
      uint32_t kind = rnd() % 10;
      /* the ring side may prefetch any number of times before any operation */
      for (uint32_t k = rnd() % 4; k > 0; k--) { segacd_cdda_prefetch(); prefetch_total++; }
      static uint8_t a[9000], b[9000];
      if (kind < 6) {                       /* audio read of a frame's worth, 1..3000 bytes */
        int want = (int)(1 + rnd() % 3000) & ~3; if (want == 0) want = 4;
        g_hooks_on = 0;
        int ra = (int)pm_read_audio(a, want, &pref);
        g_hooks_on = 1;
        int rb = gnw_cdda_read(b, want, &prg);
        if (ra != rb || memcmp(a, b, (size_t)ra)) { failures++; printf("FAIL seed %u op %d: audio read %d: ref %d ring %d\n", seed, op, want, ra, rb); goto next; }
      } else if (kind < 8) {                /* data path reads the shared fd */
        int want = (int)(1 + rnd() % 2400); foreign_total++;
        g_hooks_on = 0; int ra = (int)pm_read(a, want, &pref); g_hooks_on = 1;
        int rb = (int)pm_read(b, want, &prg);
        if (ra != rb || memcmp(a, b, (size_t)ra)) { failures++; printf("FAIL seed %u op %d: foreign read %d: ref %d ring %d\n", seed, op, want, ra, rb); goto next; }
      } else if (kind == 8) {               /* data path seeks (absolute or relative) */
        long off = (long)(rnd() % (FILE_SIZE - 100)); int cur = (int)(rnd() & 1); foreign_total++;
        if (cur) off = (long)(rnd() % 200) - 100;
        g_hooks_on = 0; pm_seek(&pref, off, cur ? SEEK_CUR : 0); g_hooks_on = 1;
        pm_seek(&prg, off, cur ? SEEK_CUR : 0);
      } else {                              /* a new CD-DA start: audio seeks and re-registers */
        long off = (long)(rnd() % (FILE_SIZE - 3000));
        g_hooks_on = 0; pm_seek(&pref, off, 0); g_hooks_on = 1;
        pm_seek(&prg, off, 0);
        gnw_cdda_set_stream(&prg);
      }
      ops_total++;
    }
    next:;
  }
  /* a CHD-typed stream must never get a ring */
  { pm_file chd = { &(fake_t){{0}, 0}, 0, 0, PMT_CHD }; gnw_cdda_set_stream(&chd);
    if (segacd_cdda.stream != NULL) { failures++; printf("FAIL: a non-uncompressed stream got a ring\n"); } }
  printf("segacd_cdda_ring: %d ops (%d foreign, %d prefetch calls), %d failures\n", ops_total, foreign_total, prefetch_total, failures);
  return failures ? 1 : 0;
}
