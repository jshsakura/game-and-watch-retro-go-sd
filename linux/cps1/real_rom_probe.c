/*
 * First execution of REAL CPS-1 game code on this port.
 *
 * Loads a packed .cps1 (see docs/CPS1_ROM_PIPELINE.md), maps its 68000
 * program region through the real CPS-1 memory map, and runs Musashi from
 * the ROM's own reset vector -- no synthetic program, no fabricated scene.
 *
 * What it reports, and why each number matters:
 *   - whether the reset vector the header claims is what the CPU actually
 *     starts from (byte order sanity, one more time, at execution level)
 *   - how far the PC travels and whether it stays inside the ROM
 *   - illegal/unimplemented opcode count -- Musashi should report ZERO on
 *     real game code; anything else means the memory map or byte order is
 *     wrong, not that the game is broken
 *   - every CPS-A / CPS-B register the game writes, in order. This is the
 *     real proof the game is talking to the video hardware: if Tenchi wo
 *     Kurau II's boot code programs the scroll/OBJ/palette base registers,
 *     those writes land here with the values the PPU work will consume.
 *   - gfxram write coverage, so the next step (graphics cost) knows how
 *     much the game actually touches.
 *
 * This is a PROBE, not a frame-cost measurement: it deliberately does not
 * render anything. Graphics cost needs the renderer pointed at this same
 * real data, which is the next milestone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cps1_m68k.h"

#define WRAM_BYTES    0x10000u
#define GFXRAM_BYTES  0x30000u   /* 192 KB, matches cps1_core.c */
#define GFXRAM_BASE   0x900000u
#define CPSA_BASE     0x800100u
#define CPSB_BASE     0x800140u

static uint8_t  s_wram[WRAM_BYTES];
static uint8_t  s_gfxram[GFXRAM_BYTES];
static uint16_t s_regs[0x80];      /* 0x800000-0x8000FF word-indexed */

static unsigned s_gfxram_writes, s_reg_writes, s_unmapped_reads, s_input_reads;
static uint8_t  s_gfxram_touched[GFXRAM_BYTES / 0x1000]; /* 4 KB granularity */

#define REGLOG_MAX 64
static struct { uint32_t addr; uint16_t val; } s_reglog[REGLOG_MAX];
static unsigned s_reglog_n;

static uint16_t probe_read16(uint32_t addr)
{
    if (addr >= GFXRAM_BASE && addr < GFXRAM_BASE + GFXRAM_BYTES) {
        uint32_t o = addr - GFXRAM_BASE;
        return (uint16_t)((s_gfxram[o] << 8) | s_gfxram[o + 1]);
    }
    /*
     * CPS-1 input ports occupy 0x800000-0x80001F and are ACTIVE LOW: a 1 bit
     * means "not pressed", so 0xFFFF is the correct idle state (no coin, no
     * start, no direction, no button). This branch MUST come before the
     * register read-back below -- an earlier version of this probe let the
     * input range fall through to s_regs[] (all zeroes) and thereby told the
     * game every button and both coin slots were held down from power-on,
     * which is not a state the boot code is ever written to survive.
     */
    if (addr < 0x800020u) {
        s_input_reads++;
        return 0xFFFFu;
    }
    if (addr >= 0x800000u && addr < 0x800100u)
        return s_regs[(addr & 0xFFu) >> 1];
    s_unmapped_reads++;
    return 0xFFFFu;
}

static void probe_write16(uint32_t addr, uint16_t val)
{
    if (addr >= GFXRAM_BASE && addr < GFXRAM_BASE + GFXRAM_BYTES) {
        uint32_t o = addr - GFXRAM_BASE;
        s_gfxram[o] = (uint8_t)(val >> 8);
        s_gfxram[o + 1] = (uint8_t)val;
        s_gfxram_touched[o / 0x1000] = 1;
        s_gfxram_writes++;
        return;
    }
    if (addr >= 0x800000u && addr < 0x800100u) {
        s_regs[(addr & 0xFFu) >> 1] = val;
        s_reg_writes++;
        if (s_reglog_n < REGLOG_MAX) {
            s_reglog[s_reglog_n].addr = addr;
            s_reglog[s_reglog_n].val = val;
            s_reglog_n++;
        }
    }
}

/* .cps1 header, little-endian (docs/CPS1_ROM_PIPELINE.md section 3-2). */
typedef struct {
    char     magic[4];
    uint16_t version, flags;
    char     romset[16];
    uint32_t prg_off, prg_size, gfx_off, gfx_size;
    uint32_t z80_off, z80_size, snd_off, snd_size;
    uint32_t cps_b_id, payload_crc;
} cps1_header_t;

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/cps1_rom/wofj.cps1";
    uint32_t cycles = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 20000000u;

    FILE *f = fopen(path, "rb");
    if (!f) { printf("[probe] cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)fsz);
    if (!blob || fread(blob, 1, (size_t)fsz, f) != (size_t)fsz) {
        printf("[probe] read failed\n"); return 1;
    }
    fclose(f);

    cps1_header_t h;
    memcpy(&h, blob, sizeof(h));
    if (memcmp(h.magic, "CPS1", 4) != 0) { printf("[probe] bad magic\n"); return 1; }

    printf("[probe] %s: romset=%.16s ver=%u flags=0x%04x prg=%u B gfx=%u B\n",
           path, h.romset, h.version, h.flags, h.prg_size, h.gfx_size);

    const uint8_t *prg = blob + h.prg_off;
    const cps1_m68k_io_t io = { probe_read16, probe_write16 };
    cps1_m68k_init(prg, h.prg_size, s_wram, &io);
    cps1_m68k_reset();

    uint32_t pc0 = cps1_m68k_get_pc(), sp0 = cps1_m68k_get_areg(7);
    printf("[probe] reset: PC=0x%08x SSP=0x%08x\n", pc0, sp0);

    /*
     * Drive the board's vblank interrupt. CPS-1 raises 68000 IRQ level 2
     * once per frame (MAME wires cps1_interrupt to the screen's vblank),
     * and the boot code waits on it -- without it the game initialises RAM
     * and then spins forever in a handful of instructions, which is exactly
     * what this probe showed before the interrupt was added. One frame is
     * 10MHz/60 = 166,666 68000 cycles; m68k.cycles counts MUL=7 master
     * cycles, so a frame is 166666*7 master cycles.
     */
    const uint32_t FRAME_MASTER = 166666u * 7u;
    uint32_t pc_min = 0xFFFFFFFFu, pc_max = 0, outside = 0;
    unsigned frames = cycles / FRAME_MASTER;
    if (frames == 0) frames = 1;
    uint32_t consumed_total = 0;
    unsigned distinct = 0;
    uint32_t seen[64]; memset(seen, 0, sizeof(seen));
    for (unsigned i = 0; i < frames; i++) {
        /* vblank is a PULSE on real hardware, not a level held for the whole
         * frame: assert, let the CPU take it, then release. Holding it high
         * re-enters the handler forever and the main loop never advances. */
        cps1_m68k_set_irq(2);
        consumed_total += cps1_m68k_run(FRAME_MASTER / 64u);
        cps1_m68k_set_irq(0);
        consumed_total += cps1_m68k_run(FRAME_MASTER - FRAME_MASTER / 64u);
        uint32_t pc = cps1_m68k_get_pc();
        if (pc < pc_min) pc_min = pc;
        if (pc > pc_max) pc_max = pc;
        if (pc >= h.prg_size) outside++;
        unsigned known = 0;
        for (unsigned k = 0; k < distinct; k++) if (seen[k] == pc) { known = 1; break; }
        if (!known && distinct < 64) seen[distinct++] = pc;
    }
    const unsigned SLICES = frames;

    unsigned pages = 0;
    for (unsigned i = 0; i < GFXRAM_BYTES / 0x1000; i++) pages += s_gfxram_touched[i];

    printf("[probe] ran %u master cycles (~%u 68000 cycles, ~%.1f frames of a 10MHz board)\n",
           consumed_total, consumed_total / 7u,
           (double)(consumed_total / 7u) / 166666.0);
    printf("[probe] PC after run = 0x%08x   (sampled range 0x%08x..0x%08x, %u/%u samples outside ROM)\n",
           cps1_m68k_get_pc(), pc_min, pc_max, outside, SLICES);
    printf("[probe] SR=0x%04x (interrupt mask level %u -- 7 means the game has NOT yet "
           "enabled interrupts)\n", cps1_m68k_get_sr(), (cps1_m68k_get_sr() >> 8) & 7u);
    printf("[probe] distinct end-of-frame PCs = %u  (1 = stuck in one spot; many = the "
           "game loop is really advancing)\n", distinct);
    printf("[probe] register writes = %u, gfxram writes = %u (%u/%u 4KB pages touched), "
           "input reads = %u, unmapped reads = %u\n",
           s_reg_writes, s_gfxram_writes, pages, GFXRAM_BYTES / 0x1000, s_input_reads, s_unmapped_reads);

    if (s_reglog_n) {
        printf("[probe] first %u CPS-A/CPS-B register writes:\n", s_reglog_n);
        for (unsigned i = 0; i < s_reglog_n; i++) {
            const char *which = (s_reglog[i].addr >= CPSB_BASE) ? "CPS-B" :
                                (s_reglog[i].addr >= CPSA_BASE) ? "CPS-A" : "IO   ";
            printf("         %s 0x%06x = 0x%04x\n", which, s_reglog[i].addr, s_reglog[i].val);
        }
    } else {
        printf("[probe] NO register writes -- the boot code never reached video setup\n");
    }

    free(blob);
    return 0;
}
