/* What SNES_SPIN_BAKE would install, for a whole ROM library, using the
 * firmware's own code.
 *
 * The mapper decision is not a formula, it is a score across four candidate
 * header positions (snes_other.c), and the install then validates the pc it
 * computed by reading it back through cart_read(). Reimplementing either of
 * those in a script would be testing a different program -- the same disease
 * this tree has paid for repeatedly -- so this links the real snes_loadRom()
 * and the real spin_bake_scan().
 *
 *   survey <rom>...      one TSV row per ROM:
 *   name  status  type  sites  bank:pc_load/pc_branch  dp  romsize
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Include through the submodule root, not through src/: external/sm/src holds
 * a features.h of its own, and putting that directory on the include path
 * shadows glibc's for every system header included above. */
#include "src/snes/snes.h"
#include "src/snes/cart.h"
#include "src/snes/spin_bake.h"

bool snes_loadRom(Snes *snes, const uint8_t *data, int length);

/* firmware allocators the core expects */
void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *itc_malloc(size_t s) { return malloc(s); }
void *ahb_malloc(size_t s) { return malloc(s); }
void *ram_malloc(size_t s) { return malloc(s); }
void *ram_calloc(size_t n, size_t s) { return calloc(n, s); }
int  CpuOpcodeHook(uint32_t a) { (void)a; return 0; }
bool HookedFunctionRts(int l) { (void)l; return false; }
bool g_fail, g_new_ppu = true;   /* g_ppu_skip_render belongs to ppu.c */
void Die(const char *s) { (void)s; }
void Warning(const char *s) { (void)s; }
void RtlApuWrite(uint32_t a, uint8_t v) { (void)a; (void)v; }

static uint8_t wram[0x20000];

int main(int argc, char **argv) {
  printf("name\tstatus\ttype\tsites\tsite\tdp\tromsize\n");
  for (int i = 1; i < argc; i++) {
    const char *path = argv[i];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s\tOPEN_FAIL\t-\t-\t-\t-\t-\n", base); continue; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = malloc(len);
    if (!raw || fread(raw, 1, len, f) != (size_t)len) {
      printf("%s\tREAD_FAIL\t-\t-\t-\t-\t-\n", base); fclose(f); free(raw); continue;
    }
    fclose(f);

    /* the launcher's copier-header skip, verbatim (main_snes.c) */
    uint8_t *rom = raw;
    long sz = len;
    if (sz > 512 && (sz % 1024) == 512) { rom += 512; sz -= 512; }

    Snes *snes = snes_init(wram);
    bool ok = snes_loadRom(snes, rom, (int)sz);
    if (!ok) {
      printf("%s\tLOAD_FAIL\t-\t-\t-\t-\t%ld\n", base, sz);
    } else {
      spin_bake_scan(snes);
      if (g_bake.on)
        printf("%s\tOK\t%u\t%lu\t%02x:%04x/%04x\t%02x\t%lu\n", base,
               snes->cart->type, (unsigned long)g_bake.sites, g_bake.bank,
               g_bake.pc_load, g_bake.pc_branch, g_bake.dp_off,
               (unsigned long)snes->cart->romSize);
      else
        printf("%s\tNO_MATCH\t%u\t%lu\t-\t-\t%lu\n", base, snes->cart->type,
               (unsigned long)g_bake.sites, (unsigned long)snes->cart->romSize);
    }
    snes_free(snes);
    free(raw);
  }
  return 0;
}
