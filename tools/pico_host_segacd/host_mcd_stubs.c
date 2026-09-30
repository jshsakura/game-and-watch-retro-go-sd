/* Host-side stubs for the Sega CD core: the platform hooks the device porting
 * layer (Core/Src/porting/segacd/main_segacd.c) supplies, minus everything the
 * LCD/odroid layer does that has no meaning off-device. Signatures are copied
 * from main_segacd.c exactly -- the 32X harness taught that inventing one (the
 * gnw_m68k_bank_alloc(void) affair) allocates garbage and dies at exit. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>


void  PicoDrawSetOutputSMS(int m) { (void)m; }
void  PicoDoHighPal555SMS(void) {}
void  PicoDraw2SetOutBuf(void *dest, int increment) { (void)dest; (void)increment; }
void  PicoDraw2Init(void) { PicoDraw2SetOutBuf(NULL, 0); }
void  PicoFrameFull(void) {}
void  emu_video_mode_change(int sl, int lc, int sc, int cc) { (void)sl;(void)lc;(void)sc;(void)cc; }
void  PicoSVPInit(void) {}
void  PicoSVPStartup(void) {}
void  lprintf(const char *f, ...) { va_list a; va_start(a, f); vfprintf(stderr, f, a); va_end(a); }

/* Upstream pico.c references the 32X entry points unconditionally (its
 * PicoLoopPrepare/PicoFrame have no NO_32X guards); the segacd run never
 * reaches them at runtime (AHW=PAHW_MCD, POPT_EN_32X off), so stubs satisfy
 * the link. The fork's build gc-sections these away. */
void  Pico32xInit(void) {}
void  Pico32xPrepare(void) {}
void  Pico32xPower(void) {}
void  Pico32xReset(void) {}
void  Pico32xFrame(void) {}
unsigned int PicoRead8_32x(unsigned int a)  { (void)a; return 0; }
unsigned int PicoRead16_32x(unsigned int a) { (void)a; return 0; }
void  PicoWrite8_32x(unsigned int a, unsigned int d)  { (void)a;(void)d; }
void  PicoWrite16_32x(unsigned int a, unsigned int d) { (void)a;(void)d; }

/* Upstream's sound.c expects the frontend to own the YM2413 instance
 * (pandora/linux main.c define it); the fork's sound code does not. */
#ifndef GNW_MCD_SPLIT
#include "pico/sound/emu2413/emu2413.h"
OPLL *opll;
int ym2413_pack_state(void *buf, size_t size) { (void)buf;(void)size; return 0; }
int ym2413_unpack_state(const void *buf, size_t size) { (void)buf;(void)size; return 0; }
#endif

/* The device maps tagged pools (LCD bonus, AHB, AXI, DTCM, SRAM4) at fixed tag
 * addresses -- see mcd_alloc_split_ram in pico/cd/mcd.c. On the host every tag
 * is just heap; the split is a device memory-map concern, not a logic one. */
void *plat_mmap(unsigned long a, size_t s, int e, int f) { (void)a;(void)e;(void)f; return malloc(s); }
void *plat_mremap(void *p, size_t o, size_t n) { (void)o; return realloc(p, n); }
void  plat_munmap(void *p, size_t s) { (void)s; free(p); }
int   plat_mem_set_exec(void *p, size_t s) { (void)p;(void)s; return 0; }

/* mp3 tracks: the converted cue/bin library is raw 2352-sector audio, so these
 * paths are never taken (same stubs the device carries). */
int   mp3_get_bitrate(void *f, int size) { (void)f;(void)size; return -1; }
void  mp3_start_play(void *f, int pos) { (void)f;(void)pos; }
void  mp3_update(int32_t *b, int l, int s) { (void)b;(void)l;(void)s; }

/* zip + inflate: a cue never takes these paths. */
void *openzip(const char *p) { (void)p; return NULL; }
void *readzip(void *z) { (void)z; return NULL; }
int   seekcompresszip(void *z, void *e) { (void)z;(void)e; return -1; }
void  closezip(void *z) { (void)z; }
int   inflate(void *s, int f) { (void)s;(void)f; return -2; }
int   inflateEnd(void *s) { (void)s; return -2; }
int   inflateReset(void *s) { (void)s; return -2; }
int   inflateInit2_(void *s, int w, const char *v, int n) { (void)s;(void)w;(void)v;(void)n; return -2; }
