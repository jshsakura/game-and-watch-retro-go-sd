/* Function stubs for excluded subsystems (Mega CD, zip/chd, SH-2 DRC). No pico
 * headers here on purpose: the linker resolves by name, so plain signatures
 * avoid prototype conflicts. None execute for a raw 32X ROM in interpreter mode
 * except sh2_drc_wcheck_* (per SH-2 write) — empty is the correct interp no-op. */
#include <stdarg.h>
void lprintf(const char *fmt, ...) { (void)fmt; }
void emu_32x_startup(void) {}
void emu_video_mode_change(int a, int b, int c) { (void)a;(void)b;(void)c; }
void PicoInitMCD(void) {}
void PicoPowerMCD(void) {}
int  PicoResetMCD(void) { return 0; }
void PicoFrameMCD(void) {}
int  PicoCreateMCD(void *b, unsigned s) { (void)b;(void)s; return -1; }
void PicoMemSetupCD(void) {}
void PicoMCDPrepare(void) {}   /* PicoLoopPrepare references it; no-op for raw 32X */
unsigned PicoRead8_mcd_io(unsigned a) { (void)a; return 0; }
unsigned PicoRead16_mcd_io(unsigned a) { (void)a; return 0; }
void PicoWrite8_mcd_io(unsigned a, unsigned d) { (void)a;(void)d; }
void PicoWrite16_mcd_io(unsigned a, unsigned d) { (void)a;(void)d; }
void pcd_run_cpus(int c) { (void)c; }
void pcd_sync_s68k(unsigned c, int r) { (void)c;(void)r; }
void pcd_prepare_frame(void) {}
void pcd_pcm_update(int *b, int l, int s) { (void)b;(void)l;(void)s; }
unsigned pcd_base_address(unsigned b) { (void)b; return 0; }
int  cdd_load(const char *f, int t) { (void)f;(void)t; return -1; }
void cdd_unload(void) {}
void *cue_parse(const char *f) { (void)f; return 0; }
void *chd_parse(const char *f) { (void)f; return 0; }
void cdparse_destroy(void *p) { (void)p; }
int  mp3_update(int *b, int l, int s) { (void)b;(void)l;(void)s; return 0; }
void *openzip(const char *f) { (void)f; return 0; }
void closezip(void *z) { (void)z; }
int  readzip(void *z, const char *n, void **d, unsigned *s) { (void)z;(void)n;(void)d;(void)s; return -1; }
int  seekcompresszip(void *z, void *e) { (void)z;(void)e; return -1; }
int  sh2_drc_init(void *s) { (void)s; return 0; }
void sh2_drc_finish(void *s) { (void)s; }
void sh2_drc_mem_setup(void *s) { (void)s; }
int  sh2_execute_drc(void *s, int c) { (void)s;(void)c; return 0; }
void sh2_drc_wcheck_ram(unsigned a, unsigned l, void *s) { (void)a;(void)l;(void)s; }
void sh2_drc_wcheck_da(unsigned a, unsigned l, void *s) { (void)a;(void)l;(void)s; }
