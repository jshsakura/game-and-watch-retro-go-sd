/* Sega/Mega CD bus wiring — phase 2.
 *
 * Two address spaces:
 *   SUB-68K  (SCD.sub_ctx.memory_map):
 *     $000000-$07FFFF  PRG-RAM   512 KB (direct, 8 pages)
 *     $080000-$0BFFFF  Word-RAM  256 KB (2M mode, direct — arbitration TODO ph2b)
 *     $FF0000-$FF7FFF  PCM window (handlers — RF5C164, ph4)
 *     $FF8000-$FFFFFF  sub gate array / CDC / CDD regs (handlers)
 *   MAIN-68K  (extends gwenesis m68k.memory_map after cart load):
 *     $020000-$03FFFF  PRG-RAM 128 KB window (bank BK0/BK1)
 *     $200000-$23FFFF  Word-RAM (2M) / bank (1M)
 *     $A12000-$A120FF  gate array registers
 *
 * gwenesis page granularity: memory_map[256], one per 64 KB. base != NULL means
 * direct base[addr & 0xFFFF]; base == NULL routes to the read/write handlers.
 * The direct-base regions set base per-page to the right sub-offset so the
 * `& 0xFFFF` index is correct across a multi-page array.
 */
#include <string.h>
#include "segacd.h"
#include "gwenesis_bus.h"

extern m68ki_cpu_core m68k;

#define PAGE_SHIFT 16
#define PAGE_SIZE  0x10000
#define PAGE(addr) (((addr) >> PAGE_SHIFT) & 0xFF)

/* ---- sub-CPU $FF0000 page: PCM (low) + gate array/CDC/CDD (high) ---- */

static unsigned int sub_ff_read8(unsigned int address)
{
    unsigned int off = address & 0xFFFF;
    if (off < 0x8000)                       /* $FF0000-$FF7FFF: PCM window */
        return SCD.pcm_ram[off & (SEGACD_PCM_RAM_SIZE - 1)];  /* TODO ph4: banked */
    return SCD.s68k_regs[off & (SEGACD_GA_REGS - 1)];         /* TODO ph3: CDC/CDD */
}

static unsigned int sub_ff_read16(unsigned int address)
{
    return (sub_ff_read8(address) << 8) | sub_ff_read8(address + 1);
}

static void sub_ff_write8(unsigned int address, unsigned int data)
{
    unsigned int off = address & 0xFFFF;
    if (off < 0x0020) {                    /* $FF0001-$FF001F: RF5C164 registers */
        segacd_pcm_write(off >> 1, data);
    } else if (off >= 0x2000 && off < 0x4000) {   /* $FF2000-$FF3FFF: 4 KB wave-RAM window */
        unsigned int a = (unsigned)SCD.pcm.bank * 0x1000 + (off & 0x0FFF);
        SCD.pcm_ram[a & (SEGACD_PCM_RAM_SIZE - 1)] = (uint8_t)data;
    } else if (off >= 0x8000) {            /* $FF8000+: gate array / CDC / CDD */
        SCD.s68k_regs[off & (SEGACD_GA_REGS - 1)] = (uint8_t)data;
    }
}

static void sub_ff_write16(unsigned int address, unsigned int data)
{
    sub_ff_write8(address, data >> 8);
    sub_ff_write8(address + 1, data & 0xFF);
}

/* Build the sub-CPU's 256-entry memory_map into SCD.sub_ctx. */
void segacd_sub_build_memory_map(void)
{
    cpu_memory_map *map = SCD.sub_ctx.memory_map;
    memset(map, 0, sizeof(SCD.sub_ctx.memory_map));

    /* $000000-$07FFFF : PRG-RAM, 8 direct pages */
    for (int p = 0; p < 8; p++)
        map[p].base = SCD.prg_ram + p * PAGE_SIZE;

    /* $080000-$0BFFFF : Word-RAM (2M mode, sub owns), 4 direct pages.
     * TODO(ph2b): honor word_mode/word_owner — NULL out + handler when main owns. */
    for (int p = 0; p < 4; p++)
        map[0x08 + p].base = SCD.word_ram + p * PAGE_SIZE;

    /* $FF0000-$FFFFFF : PCM + gate array, handler page */
    map[0xFF].base   = NULL;
    map[0xFF].read8  = sub_ff_read8;
    map[0xFF].read16 = sub_ff_read16;
    map[0xFF].write8 = sub_ff_write8;
    map[0xFF].write16= sub_ff_write16;
}

/* ---- main-CPU view of CD space (handlers; PRG window is bank-selected) ---- */

static unsigned int main_prgwin_read8(unsigned int address)
{
    unsigned int off = (address & 0x1FFFF) + (unsigned)SCD.prg_bank * 0x20000;
    return SCD.prg_ram[off & (SEGACD_PRG_RAM_SIZE - 1)];
}
static unsigned int main_prgwin_read16(unsigned int address)
{
    return (main_prgwin_read8(address) << 8) | main_prgwin_read8(address + 1);
}
static void main_prgwin_write8(unsigned int address, unsigned int data)
{
    unsigned int off = (address & 0x1FFFF) + (unsigned)SCD.prg_bank * 0x20000;
    SCD.prg_ram[off & (SEGACD_PRG_RAM_SIZE - 1)] = (uint8_t)data;
}
static void main_prgwin_write16(unsigned int address, unsigned int data)
{
    main_prgwin_write8(address, data >> 8);
    main_prgwin_write8(address + 1, data & 0xFF);
}

static unsigned int main_ga_read8(unsigned int address)
{
    return SCD.s68k_regs[address & (SEGACD_GA_REGS - 1)];   /* TODO ph3: real GA */
}
static unsigned int main_ga_read16(unsigned int address)
{
    return (main_ga_read8(address) << 8) | main_ga_read8(address + 1);
}
static void main_ga_write8(unsigned int address, unsigned int data)
{
    unsigned int reg = address & (SEGACD_GA_REGS - 1);
    SCD.s68k_regs[reg] = (uint8_t)data;

    switch (address & 0xFFF) {
    case 0x001:
        /* $A12001: bit0 SRES (0=sub in reset, 1=run), bit1 SBRQ (1=main holds
         * sub bus). Sub runs only when released AND its bus is granted. */
        if ((data & 0x01) && !(data & 0x02)) {
            if (!SCD.sub_running) segacd_sub_release();
        } else {
            segacd_sub_hold();
        }
        break;
    case 0x003:
        /* PRG-RAM 128 KB bank select (BK0/BK1) + Word-RAM mode/owner bits. */
        SCD.prg_bank  = (uint8_t)((data >> 6) & 3);
        SCD.word_mode = (uint8_t)((data >> 2) & 1);   /* DMNA/MODE — refine ph2b */
        break;
    }
}
static void main_ga_write16(unsigned int address, unsigned int data)
{
    main_ga_write8(address, data >> 8);
    main_ga_write8(address + 1, data & 0xFF);
}

/* Map the region BIOS as the MAIN-CPU boot ROM at $000000-$01FFFF (2 pages).
 * `bios` is read-only — a flash-XIP pointer on device (0 RAM), or a RAM buffer.
 * The Mega CD boots the main 68K from BIOS, not the cartridge. */
void segacd_map_bios(const uint8_t *bios)
{
    if (!bios) return;
    for (int p = 0x00; p <= 0x01; p++) {
        m68k.memory_map[p].base   = (unsigned char *)bios + p * PAGE_SIZE;
        m68k.memory_map[p].read8  = NULL;
        m68k.memory_map[p].read16 = NULL;
        /* BIOS is read-only: leave write handlers NULL (writes ignored). */
        m68k.memory_map[p].write8 = NULL;
        m68k.memory_map[p].write16= NULL;
    }
}

/* Patch the MAIN gwenesis memory_map for CD regions. Call AFTER
 * load_cartridge()/gwenesis_bus_init_memory_map() so we override cart pages. */
void segacd_main_map_cd_space(void)
{
    cpu_memory_map *map = m68k.memory_map;

    /* $020000-$03FFFF : PRG-RAM 128 KB window (2 pages) */
    for (int p = 0x02; p <= 0x03; p++) {
        map[p].base   = NULL;
        map[p].read8  = main_prgwin_read8;
        map[p].read16 = main_prgwin_read16;
        map[p].write8 = main_prgwin_write8;
        map[p].write16= main_prgwin_write16;
    }

    /* $200000-$23FFFF : Word-RAM (2M mode, main owns after reset), 4 pages.
     * TODO(ph2b): arbitration — NULL + handler when sub owns. */
    for (int p = 0; p < 4; p++)
        map[0x20 + p].base = SCD.word_ram + p * PAGE_SIZE;

    /* $A12000 page : gate array registers (shares page $A1 with other I/O —
     * install handlers; they only claim the $A120xx sub-range). */
    map[0xA1].base   = NULL;
    map[0xA1].read8  = main_ga_read8;
    map[0xA1].read16 = main_ga_read16;
    map[0xA1].write8 = main_ga_write8;
    map[0xA1].write16= main_ga_write16;
}
