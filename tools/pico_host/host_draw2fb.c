/* Same binding the QEMU rig provides (tools/m7_qemu_rig/rig_32x_draw2fb.c):
 * the trimmed 32X source set has no owner for Draw2FB, and leaving it NULL
 * segfaults the compositor the moment 32X startup fires. */
#include "pico/pico_int.h"
#define D2FB_LINE_WIDTH 328
static unsigned char host_draw2fb[D2FB_LINE_WIDTH * (8 + 240 + 8) + 8];
void PicoDraw2SetOutBuf(void *dest, int incr)
{
  if (dest) { Pico.est.Draw2FB = dest; Pico.est.Draw2Width = incr; }
  else      { Pico.est.Draw2FB = host_draw2fb; Pico.est.Draw2Width = D2FB_LINE_WIDTH; }
}
