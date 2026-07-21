/* The trimmed 32X set excludes pico/draw2.c (the MD ALT renderer), but the 32X
 * compositor still needs the 328x(8+240+8) CLUT frame it owned:
 * PicoDrawSetOutFormat32x(PDF_RGB555) points the MD renderer's internal line
 * buffer (HighCol) INTO Pico.est.Draw2FB, and do_loop_* in pico/32x/draw.c
 * reads it back per-pixel (pmd) for MD/32X layer priority. A no-op
 * PicoDraw2SetOutBuf stub leaves Draw2FB == NULL and the compositor reads
 * wild memory (host: SIGSEGV at pmd=0xa48; device: Hardfault / garbage).
 *
 * This is draw2.c's binding, verbatim, minus the renderer:
 * PicoDrawSetOutFormat -> PicoDraw2SetOutBuf(NULL, 0) -> bind the static
 * frame. The device porting layer needs this same shim (main_md32x.c's
 * current stub is the no-op). */
#include "pico/pico_int.h"

#define D2FB_LINE_WIDTH 328
static unsigned char rig_draw2fb[D2FB_LINE_WIDTH * (8 + 240 + 8) + 8];

void PicoDraw2SetOutBuf(void *dest, int incr)
{
  if (dest) {
    Pico.est.Draw2FB = dest;
    Pico.est.Draw2Width = incr;
  } else {
    Pico.est.Draw2FB = rig_draw2fb;
    Pico.est.Draw2Width = D2FB_LINE_WIDTH;
  }
}
