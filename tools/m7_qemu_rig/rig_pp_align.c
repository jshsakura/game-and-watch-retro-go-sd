/* gnw_line_pp() on a real Cortex-M7 instruction stream, with the 32X source
 * pointer at every even alignment.
 *
 * Why this is a rig test and not a host test: the fault is a machine-code
 * fusion. The eight-pixel path reads ((u32 *)p32x)[0] and [1] back to back, and
 * arm-none-eabi-gcc fuses them into one LDRD. Cortex-M7 traps an LDRD unless the
 * address is word aligned, and the path was guarded only for a 2-byte aligned
 * p32x. A host compiler emits two plain loads, so no host sanitizer can see it
 * (Kolibri crashed on the device with Usagefault, CFSR UNALIGNED, at
 * gnw_line_pp+0x3d0, `ldrd r4, r5, [r1]`, 2026-10-03).
 *
 * The test includes the REAL draw.c and calls the real static function, so what
 * runs is the code the overlay links, built with the device's flags.
 * Success is the line PASS on semihosted stdout; a fault leaves the CPU in
 * the default handler and the runner's timeout turns that into a failure. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pico/32x/draw.c"

#define LINE 320
#define MDBG 0x21

static unsigned char src[LINE + 16] __attribute__((aligned(8)));
static unsigned char md[LINE + 16] __attribute__((aligned(8)));
static unsigned short dst[LINE + 16] __attribute__((aligned(8)));
static unsigned short pal[256];

static int run_case(int src_off)
{
  unsigned char *p32x = src + src_off;
  int k, bad = 0;

  /* No four equal neighbours (that would take the solid-run path) and every
   * MD byte is background, so the line goes through the MD-background loop. */
  for (k = 0; k < (int)sizeof(src); k++) src[k] = (unsigned char)(k * 7 + 3);
  memset(md, MDBG, sizeof(md));
  memset(dst, 0, sizeof(dst));
  for (k = 0; k < 256; k++) pal[k] = (unsigned short)(0x1000 + k * 3);

  gnw_line_pp(dst, p32x, md, pal, pal, MDBG, GNW_PP_MD_NONE);

  for (k = 0; k < LINE; k++) {
    unsigned char px = *(unsigned char *)MEM_BE2((uintptr_t)(p32x + k));
    if (dst[k] != pal[px]) {
      if (bad++ < 3) printf("  src+%d pixel %d: got %04x want %04x\n",
                            src_off, k, dst[k], pal[px]);
    }
  }
  printf("src+%d (p32x mod 4 = %u): %s\n", src_off,
         (unsigned)((uintptr_t)p32x & 3), bad ? "MISMATCH" : "ok");
  return bad;
}

int main(void)
{
  int bad = 0;
  *(volatile uint32_t *)0xE000ED24 |= (1u << 18);   /* USGFAULTENA */
  bad += run_case(0);   /* word aligned: always worked        */
  bad += run_case(2);   /* 2 mod 4: the Kolibri crash         */
  bad += run_case(4);
  bad += run_case(6);
  printf(bad ? "FAIL\n" : "PASS\n");
  return bad != 0;
}
