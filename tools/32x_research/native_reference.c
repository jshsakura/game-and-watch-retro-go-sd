/* Actual current interpreter, with unreachable buses/IRQ delivery trapped.
 * The ALU-only proof must never call one of these stubs. */
#include <stdio.h>
#include <stdlib.h>
#include <cpu/sh2/mame/sh2pico.c>

unsigned int gnw_sh2_rom_fetch_mask;
struct gnw_fetch_win gnw_fw_rom;

void proof_bind_rom(const uint16_t *rom, unsigned int size)
{
    gnw_fw_rom.region = 0x02000000;
    gnw_fw_rom.mask = size - 2;
    gnw_fw_rom.base = (unsigned char *)rom;
    gnw_sh2_rom_fetch_mask = size - 2;
}

static void unexpected(const char *what)
{
    fprintf(stderr, "Unexpected %s in register-only proof\n", what);
    exit(1);
}
u32 p32x_sh2_read8(u32 a, SH2 *s) {
#ifdef PROOF_SDRAM_BYTE_READ
    /* Exact GNW SDRAM byte fast path from pico/32x/memory.c. */
    if ((a & 0xdf000000u) == 0x06000000u)
        return ((u8 *)s->p_sdram)[(a & 0x3ffffu) ^ 1];
#endif
    (void)a; (void)s; unexpected("read8"); return 0;
}
u32 p32x_sh2_read16(u32 a, SH2 *s) { (void)a; (void)s; unexpected("read16"); return 0; }
u32 p32x_sh2_read32(u32 a, SH2 *s) { (void)a; (void)s; unexpected("read32"); return 0; }
void p32x_sh2_write8(u32 a, u32 d, SH2 *s) { (void)a; (void)d; (void)s; unexpected("write8"); }
void p32x_sh2_write16(u32 a, u32 d, SH2 *s) { (void)a; (void)d; (void)s; unexpected("write16"); }
void p32x_sh2_write32(u32 a, u32 d, SH2 *s) { (void)a; (void)d; (void)s; unexpected("write32"); }
void sh2_do_irq(SH2 *s, int level, int vector) { (void)s; (void)level; (void)vector; unexpected("IRQ"); }
void lprintf(const char *format, ...) { (void)format; unexpected("illegal opcode/log"); }
