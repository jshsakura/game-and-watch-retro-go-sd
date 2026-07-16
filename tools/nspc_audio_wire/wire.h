/* N-SPC sound-HLE live wiring (see wire.c). */
#ifndef NSPC_WIRE_H
#define NSPC_WIRE_H
#include <stdint.h>

struct Snes;

extern int g_wire_on;          /* 1 after the swap to the native player */
extern int g_wire_enable;      /* master switch; 0 = pure LLE forever */
extern const char *g_wire_variant;

void wire_apu_write(struct Snes *snes, uint32_t adr, uint8_t val);
int  wire_try_swap(struct Snes *snes, int frame);
void wire_frame_audio(int16_t *buf, int n);
int  wire_pre_opcode(struct Snes *snes);   /* $80:8028 upload HLE; 0 = not hooked */

#endif
