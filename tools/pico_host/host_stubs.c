/* Host-side stubs for the trimmed 32X core: the platform hooks the device
 * porting layer supplies, plus the zip/zlib entry points the cart loader
 * references but never reaches for a raw .32x image. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>


void  PicoDrawSetOutputSMS(int m) { (void)m; }
void  emu_video_mode_change(int s, int l, int m) { (void)s; (void)l; (void)m; }
void  lprintf(const char *f, ...) { va_list a; va_start(a, f); vfprintf(stderr, f, a); va_end(a); }

void *plat_mmap(unsigned long a, size_t s, int e, int f) { (void)a;(void)e;(void)f; return malloc(s); }
void *plat_mremap(void *p, size_t o, size_t n) { (void)o; return realloc(p, n); }
void  plat_munmap(void *p, size_t s) { (void)s; free(p); }

/* The device allocates the 68K bank window out of its own pool. The real
 * prototype takes NO argument -- declaring one here allocated a garbage-sized
 * buffer from whatever was in the register and the core wrote a full bank into
 * it, which showed up as "malloc(): corrupted top size" at exit. The 68K sees
 * 0x900000-0x9fffff, so give it the whole megabyte. */
unsigned char *gnw_m68k_bank_alloc(void) { return calloc(1, 1024 * 1024); }

/* zip + inflate: a raw .32x never takes these paths. */
void *openzip(const char *p) { (void)p; return NULL; }
void *readzip(void *z) { (void)z; return NULL; }
int   seekcompresszip(void *z, void *e) { (void)z; (void)e; return -1; }
void  closezip(void *z) { (void)z; }
int   inflate(void *s, int f) { (void)s; (void)f; return -2; }
int   inflateEnd(void *s) { (void)s; return -2; }
int   inflateReset(void *s) { (void)s; return -2; }
int   inflateInit2_(void *s, int w, const char *v, int n) { (void)s;(void)w;(void)v;(void)n; return -2; }
