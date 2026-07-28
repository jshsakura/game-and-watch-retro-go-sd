/* Host implementations of the common GBA BIOS SWIs.
 *
 * Algorithms follow the open Normmatt/VBA-M BIOS (external/gpsp/bios/), adapted
 * to gpSP's memory accessors so I/O and waitstate side effects still fire.
 * Cycle costs are approximate but proportional to work — large enough that a
 * game waiting on a CpuFastSet still sees timers and HBlank progress, small
 * enough that we do not invent multi-frame stalls. Long transfers park the CPU
 * in CPU_DMA sleep for any cost that does not fit the current slice (same
 * mechanism as a real DMA), so update_gba keeps advancing the machine.
 */
#include "gba_bios_hle.h"

#include <stdint.h>
#include <string.h>

/* gpSP internals — declared by hand; see m4a_gpsp.c for the same pattern. */
extern unsigned char *memory_map_read[8 * 1024];
extern unsigned char  ws_cyc_nseq[16][2];
extern unsigned char  ws_cyc_seq[16][2];
extern unsigned int   read_memory8(unsigned int address);
extern unsigned int   read_memory16(unsigned int address);
extern unsigned int   read_memory32(unsigned int address);
extern unsigned int   write_memory8(unsigned int address, unsigned char value);
extern unsigned int   write_memory16(unsigned int address, unsigned short value);
extern unsigned int   write_memory32(unsigned int address, unsigned int value);

#define R8(a)   ((uint8_t)read_memory8(a))
#define R16(a)  ((uint16_t)read_memory16(a))
#define R32(a)  ((uint32_t)read_memory32(a))
#define W8(a,v)  write_memory8((a), (uint8_t)(v))
#define W16(a,v) write_memory16((a), (uint16_t)(v))
#define W32(a,v) write_memory32((a), (uint32_t)(v))

/* Source must not live in the BIOS mirror (same guard as the open BIOS). */
static int src_ok(uint32_t source, uint32_t bytes)
{
    if ((source & 0xe000000u) == 0)
        return 0;
    if (((source + bytes) & 0xe000000u) == 0)
        return 0;
    return 1;
}

static int iabs32(int32_t x) { return x < 0 ? -x : x; }

static int cost_rw32(uint32_t addr, int seq)
{
    unsigned r = (addr >> 24) & 15u;
    return seq ? ws_cyc_seq[r][1] : ws_cyc_nseq[r][1];
}

static int cost_rw16(uint32_t addr, int seq)
{
    unsigned r = (addr >> 24) & 15u;
    return seq ? ws_cyc_seq[r][0] : ws_cyc_nseq[r][0];
}

/* Host pointer into EWRAM/IWRAM only — anywhere else has side effects and must
 * go through write_memory*. `nbytes` must fit the current 32 KB gpSP page. */
static uint8_t *map_ram(uint32_t addr, uint32_t nbytes)
{
    unsigned r = addr >> 24;
    unsigned char *page;
    uint32_t off;

    if (r != 2u && r != 3u)
        return 0;
    page = memory_map_read[addr >> 15];
    if (!page)
        return 0;
    off = addr & 0x7FFFu;
    if (off + nbytes > 0x8000u)
        return 0;
    return page + off;
}

/* Bulk copy/fill in EWRAM/IWRAM via memcpy. Returns 0 if any byte falls outside
 * plain RAM (caller falls back to the word-at-a-time path). */
static int ram_copy32(uint32_t src, uint32_t dst, int words, int fill, uint32_t fillv)
{
    while (words > 0) {
        uint32_t span_s = 0x8000u - (src & 0x7FFFu);
        uint32_t span_d = 0x8000u - (dst & 0x7FFFu);
        uint32_t span = span_s < span_d ? span_s : span_d;
        int nwords = (int)(span / 4u);
        uint8_t *d;
        if (nwords > words)
            nwords = words;
        if (nwords <= 0)
            return 0;
        d = map_ram(dst, (uint32_t)nwords * 4u);
        if (!d)
            return 0;
        if (fill) {
            uint32_t *p = (uint32_t *)(void *)d;
            int i;
            for (i = 0; i < nwords; i++)
                p[i] = fillv;
        } else {
            uint8_t *s = map_ram(src, (uint32_t)nwords * 4u);
            if (!s)
                return 0;
            memcpy(d, s, (size_t)nwords * 4u);
            src += (uint32_t)nwords * 4u;
        }
        dst += (uint32_t)nwords * 4u;
        words -= nwords;
    }
    return 1;
}

/* ---------------------------------------------------------------- math ---- */

static void hle_div(unsigned *regs, int arm_order, int *cycles)
{
    int32_t num, den;
    if (arm_order) {
        den = (int32_t)regs[0];
        num = (int32_t)regs[1];
    } else {
        num = (int32_t)regs[0];
        den = (int32_t)regs[1];
    }
    if (den == 0)
        return; /* decline — real BIOS hangs */

    int32_t quot = num / den;
    int32_t rem  = num % den;
    regs[0] = (unsigned)quot;
    regs[1] = (unsigned)rem;
    regs[3] = (unsigned)(quot < 0 ? -quot : quot);
    *cycles = arm_order ? 103 : 100;
}

static void hle_sqrt(unsigned *regs, int *cycles)
{
    uint32_t n = regs[0], root = 0, try_;
#define ITER(N) do { \
        try_ = root + (1u << (N)); \
        if (n >= try_ << (N)) { n -= try_ << (N); root |= 2u << (N); } \
    } while (0)
    ITER(15); ITER(14); ITER(13); ITER(12);
    ITER(11); ITER(10); ITER(9);  ITER(8);
    ITER(7);  ITER(6);  ITER(5);  ITER(4);
    ITER(3);  ITER(2);  ITER(1);  ITER(0);
#undef ITER
    regs[0] = root >> 1;
    *cycles = 120;
}

static const int16_t sine_table[256] = {
    0x0000,0x0192,0x0323,0x04B5,0x0645,0x07D5,0x0964,0x0AF1,
    0x0C7C,0x0E05,0x0F8C,0x1111,0x1294,0x1413,0x158F,0x1708,
    0x187D,0x19EF,0x1B5D,0x1CC6,0x1E2B,0x1F8B,0x20E7,0x223D,
    0x238E,0x24DA,0x261F,0x275F,0x2899,0x29CD,0x2AFA,0x2C21,
    0x2D41,0x2E5A,0x2F6B,0x3076,0x3179,0x3274,0x3367,0x3453,
    0x3536,0x3612,0x36E5,0x37AF,0x3871,0x392A,0x39DA,0x3A82,
    0x3B20,0x3BB6,0x3C42,0x3CC5,0x3D3E,0x3DAE,0x3E14,0x3E71,
    0x3EC5,0x3F0E,0x3F4E,0x3F84,0x3FB1,0x3FD3,0x3FEC,0x3FFB,
    0x4000,0x3FFB,0x3FEC,0x3FD3,0x3FB1,0x3F84,0x3F4E,0x3F0E,
    0x3EC5,0x3E71,0x3E14,0x3DAE,0x3D3E,0x3CC5,0x3C42,0x3BB6,
    0x3B20,0x3A82,0x39DA,0x392A,0x3871,0x37AF,0x36E5,0x3612,
    0x3536,0x3453,0x3367,0x3274,0x3179,0x3076,0x2F6B,0x2E5A,
    0x2D41,0x2C21,0x2AFA,0x29CD,0x2899,0x275F,0x261F,0x24DA,
    0x238E,0x223D,0x20E7,0x1F8B,0x1E2B,0x1CC6,0x1B5D,0x19EF,
    0x187D,0x1708,0x158F,0x1413,0x1294,0x1111,0x0F8C,0x0E05,
    0x0C7C,0x0AF1,0x0964,0x07D5,0x0645,0x04B5,0x0323,0x0192,
    0x0000,(int16_t)0xFE6E,(int16_t)0xFCDD,(int16_t)0xFB4B,
    (int16_t)0xF9BB,(int16_t)0xF82B,(int16_t)0xF69C,(int16_t)0xF50F,
    (int16_t)0xF384,(int16_t)0xF1FB,(int16_t)0xF074,(int16_t)0xEEEF,
    (int16_t)0xED6C,(int16_t)0xEBED,(int16_t)0xEA71,(int16_t)0xE8F8,
    (int16_t)0xE783,(int16_t)0xE611,(int16_t)0xE4A3,(int16_t)0xE33A,
    (int16_t)0xE1D5,(int16_t)0xE075,(int16_t)0xDF19,(int16_t)0xDDC3,
    (int16_t)0xDC72,(int16_t)0xDB26,(int16_t)0xD9E1,(int16_t)0xD8A1,
    (int16_t)0xD767,(int16_t)0xD633,(int16_t)0xD506,(int16_t)0xD3DF,
    (int16_t)0xD2BF,(int16_t)0xD1A6,(int16_t)0xD095,(int16_t)0xCF8A,
    (int16_t)0xCE87,(int16_t)0xCD8C,(int16_t)0xCC99,(int16_t)0xCBAD,
    (int16_t)0xCACA,(int16_t)0xC9EE,(int16_t)0xC91B,(int16_t)0xC851,
    (int16_t)0xC78F,(int16_t)0xC6D6,(int16_t)0xC626,(int16_t)0xC57E,
    (int16_t)0xC4E0,(int16_t)0xC44A,(int16_t)0xC3BE,(int16_t)0xC33B,
    (int16_t)0xC2C2,(int16_t)0xC252,(int16_t)0xC1EC,(int16_t)0xC18F,
    (int16_t)0xC13B,(int16_t)0xC0F2,(int16_t)0xC0B2,(int16_t)0xC07C,
    (int16_t)0xC04F,(int16_t)0xC02D,(int16_t)0xC014,(int16_t)0xC005,
    (int16_t)0xC000,(int16_t)0xC005,(int16_t)0xC014,(int16_t)0xC02D,
    (int16_t)0xC04F,(int16_t)0xC07C,(int16_t)0xC0B2,(int16_t)0xC0F2,
    (int16_t)0xC13B,(int16_t)0xC18F,(int16_t)0xC1EC,(int16_t)0xC252,
    (int16_t)0xC2C2,(int16_t)0xC33B,(int16_t)0xC3BE,(int16_t)0xC44A,
    (int16_t)0xC4E0,(int16_t)0xC57E,(int16_t)0xC626,(int16_t)0xC6D6,
    (int16_t)0xC78F,(int16_t)0xC851,(int16_t)0xC91B,(int16_t)0xC9EE,
    (int16_t)0xCACA,(int16_t)0xCBAD,(int16_t)0xCC99,(int16_t)0xCD8C,
    (int16_t)0xCE87,(int16_t)0xCF8A,(int16_t)0xD095,(int16_t)0xD1A6,
    (int16_t)0xD2BF,(int16_t)0xD3DF,(int16_t)0xD506,(int16_t)0xD633,
    (int16_t)0xD767,(int16_t)0xD8A1,(int16_t)0xD9E1,(int16_t)0xDB26,
    (int16_t)0xDC72,(int16_t)0xDDC3,(int16_t)0xDF19,(int16_t)0xE075,
    (int16_t)0xE1D5,(int16_t)0xE33A,(int16_t)0xE4A3,(int16_t)0xE611,
    (int16_t)0xE783,(int16_t)0xE8F8,(int16_t)0xEA71,(int16_t)0xEBED,
    (int16_t)0xED6C,(int16_t)0xEEEF,(int16_t)0xF074,(int16_t)0xF1FB,
    (int16_t)0xF384,(int16_t)0xF50F,(int16_t)0xF69C,(int16_t)0xF82B,
    (int16_t)0xF9BB,(int16_t)0xFB4B,(int16_t)0xFCDD,(int16_t)0xFE6E
};

static uint32_t arctan_body(uint32_t input)
{
    int32_t a = -(((int32_t)(input * input)) >> 14);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (uint32_t)(((int32_t)input * b) >> 16);
}

static void hle_arctan(unsigned *regs, int *cycles)
{
    regs[0] = arctan_body(regs[0]);
    *cycles = 80;
}

static void hle_arctan2(unsigned *regs, int *cycles)
{
    int32_t x = (int32_t)regs[0];
    int32_t y = (int32_t)regs[1];
    uint32_t res = 0;

    if (y == 0) {
        res = ((uint32_t)x >> 16) & 0x8000u;
    } else if (x == 0) {
        res = (((uint32_t)y >> 16) & 0x8000u) + 0x4000u;
    } else {
        int ax = iabs32(x), ay = iabs32(y);
        if (ax > ay || (ax == ay && !((x < 0) && (y < 0)))) {
            /* Div(y<<14, x) then ArcTan — reuse host division */
            int32_t div = (y << 14) / x;
            uint32_t at = arctan_body((uint32_t)div);
            if (x < 0)
                res = 0x8000u + at;
            else
                res = ((((uint32_t)y >> 16) & 0x8000u) << 1) + at;
        } else {
            int32_t div = (x << 14) / y;
            uint32_t at = arctan_body((uint32_t)div);
            res = (0x4000u + (((uint32_t)y >> 16) & 0x8000u)) - at;
        }
    }
    regs[0] = res;
    *cycles = 160;
}

/* ---------------------------------------------------------- memory copy ---- */

static int hle_cpu_set(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1], cnt = regs[2];
    int count = (int)(cnt & 0x1FFFFFu);
    int fill = (cnt >> 24) & 1;
    int word32 = (cnt >> 26) & 1;
    int cost = 24;

    if (!src_ok(source, (uint32_t)(((cnt << 11) >> 9) & 0x1fffffu)))
        return 0;

    if (word32) {
        source &= ~3u;
        dest &= ~3u;
        /* Fast path: both ends in EWRAM/IWRAM — one memcpy, same cycle bill. */
        if (fill) {
            uint32_t value = source > 0x0EFFFFFFu ? 0x1CAD1CADu : R32(source);
            cost += cost_rw32(source, 0) + count * (1 + cost_rw32(dest, 1));
            if (ram_copy32(source, dest, count, 1, value)) {
                *cycles = cost;
                return 1;
            }
            while (count--) {
                W32(dest, value);
                dest += 4;
            }
        } else {
            cost += count * (cost_rw32(source, 1) + cost_rw32(dest, 1));
            if (ram_copy32(source, dest, count, 0, 0)) {
                *cycles = cost;
                return 1;
            }
            while (count--) {
                uint32_t v = source > 0x0EFFFFFFu ? 0x1CAD1CADu : R32(source);
                W32(dest, v);
                source += 4;
                dest += 4;
            }
        }
    } else {
        if (fill) {
            uint16_t value = source > 0x0EFFFFFFu ? 0x1CADu : R16(source);
            cost += cost_rw16(source, 0);
            while (count--) {
                W16(dest, value);
                cost += 1 + cost_rw16(dest, 1);
                dest += 2;
            }
        } else {
            while (count--) {
                uint16_t v = source > 0x0EFFFFFFu ? 0x1CADu : R16(source);
                W16(dest, v);
                cost += cost_rw16(source, 1) + cost_rw16(dest, 1);
                source += 2;
                dest += 2;
            }
        }
    }
    *cycles = cost;
    return 1;
}

static int hle_cpu_fast_set(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0] & ~3u;
    uint32_t dest = regs[1] & ~3u;
    uint32_t cnt = regs[2];
    int count = (int)(cnt & 0x1FFFFFu);
    int fill = (cnt >> 24) & 1;
    int cost = 24;
    int i;

    if (!src_ok(regs[0], (uint32_t)(((cnt << 11) >> 9) & 0x1fffffu)))
        return 0;

    if (fill) {
        uint32_t value = source > 0x0EFFFFFFu ? 0xBAFFFFFBu : R32(source);
        cost += cost_rw32(source, 0) + count * (1 + cost_rw32(dest, 1));
        if (ram_copy32(source, dest, count, 1, value)) {
            *cycles = cost;
            return 1;
        }
        while (count > 0) {
            for (i = 0; i < 8; i++) {
                W32(dest, value);
                dest += 4;
            }
            count -= 8;
        }
    } else {
        cost += count * (cost_rw32(source, 1) + cost_rw32(dest, 1));
        if (ram_copy32(source, dest, count, 0, 0)) {
            *cycles = cost;
            return 1;
        }
        while (count > 0) {
            for (i = 0; i < 8; i++) {
                uint32_t v = source > 0x0EFFFFFFu ? 0xBAFFFFFBu : R32(source);
                W32(dest, v);
                source += 4;
                dest += 4;
            }
            count -= 8;
        }
    }
    *cycles = cost;
    return 1;
}

/* --------------------------------------------------------------- affine ---- */

static void hle_bg_affine_set(unsigned *regs, int *cycles)
{
    uint32_t src = regs[0], dest = regs[1];
    uint32_t num = regs[2];
    int cost = 24;

    while (num--) {
        int32_t cx = (int32_t)R32(src); src += 4;
        int32_t cy = (int32_t)R32(src); src += 4;
        int16_t dispx = (int16_t)R16(src); src += 2;
        int16_t dispy = (int16_t)R16(src); src += 2;
        int16_t rx = (int16_t)R16(src); src += 2;
        int16_t ry = (int16_t)R16(src); src += 2;
        uint16_t theta = R16(src) >> 8; src += 4;
        int32_t a = sine_table[(theta + 0x40) & 255];
        int32_t b = sine_table[theta];
        int16_t dx  = (int16_t)((rx * a) >> 14);
        int16_t dmx = (int16_t)((rx * b) >> 14);
        int16_t dy  = (int16_t)((ry * b) >> 14);
        int16_t dmy = (int16_t)((ry * a) >> 14);
        W16(dest, (uint16_t)dx);  dest += 2;
        W16(dest, (uint16_t)-dmx); dest += 2;
        W16(dest, (uint16_t)dy);  dest += 2;
        W16(dest, (uint16_t)dmy); dest += 2;
        W32(dest, (uint32_t)(cx - dx * dispx + dmx * dispy)); dest += 4;
        W32(dest, (uint32_t)(cy - dy * dispx - dmy * dispy)); dest += 4;
        cost += 100;
    }
    *cycles = cost;
}

static void hle_obj_affine_set(unsigned *regs, int *cycles)
{
    uint32_t src = regs[0], dest = regs[1];
    int num = (int)regs[2];
    int offset = (int)regs[3];
    int cost = 24;

    while (num--) {
        int16_t rx = (int16_t)R16(src); src += 2;
        int16_t ry = (int16_t)R16(src); src += 2;
        uint16_t theta = R16(src) >> 8; src += 4;
        int32_t a = sine_table[(theta + 0x40) & 255];
        int32_t b = sine_table[theta];
        int16_t dx  = (int16_t)((rx * a) >> 14);
        int16_t dmx = (int16_t)((rx * b) >> 14);
        int16_t dy  = (int16_t)((ry * b) >> 14);
        int16_t dmy = (int16_t)((ry * a) >> 14);
        W16(dest, (uint16_t)dx);   dest += offset;
        W16(dest, (uint16_t)-dmx); dest += offset;
        W16(dest, (uint16_t)dy);   dest += offset;
        W16(dest, (uint16_t)dmy);  dest += offset;
        cost += 60;
    }
    *cycles = cost;
}

/* ---------------------------------------------------------- decompress ---- */

static int hle_lz77_wram(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1];
    uint32_t header = R32(source);
    int len = (int)(header >> 8);
    int cost = 32;

    source += 4;
    if (!src_ok(source, (uint32_t)len))
        return 0;

    while (len > 0) {
        uint8_t d = R8(source++);
        cost += 2;
        for (int i = 0; i < 8; i++) {
            if (d & 0x80) {
                uint16_t data = (uint16_t)(R8(source) << 8);
                source++;
                data |= R8(source);
                source++;
                int length = (data >> 12) + 3;
                int offset = data & 0x0FFF;
                uint32_t window = dest - offset - 1;
                for (int j = 0; j < length; j++) {
                    W8(dest++, R8(window++));
                    cost += 3;
                    if (--len == 0) { *cycles = cost; return 1; }
                }
            } else {
                W8(dest++, R8(source++));
                cost += 2;
                if (--len == 0) { *cycles = cost; return 1; }
            }
            d <<= 1;
        }
    }
    *cycles = cost;
    return 1;
}

static int hle_lz77_vram(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1];
    uint32_t header = R32(source);
    int len = (int)(header >> 8);
    int byteCount = 0, byteShift = 0;
    uint32_t writeValue = 0;
    int cost = 32;

    source += 4;
    if (!src_ok(source, (uint32_t)len))
        return 0;

    while (len > 0) {
        uint8_t d = R8(source++);
        cost += 2;
        for (int i = 0; i < 8; i++) {
            if (d & 0x80) {
                uint16_t data = (uint16_t)(R8(source) << 8);
                source++;
                data |= R8(source);
                source++;
                int length = (data >> 12) + 3;
                int offset = data & 0x0FFF;
                uint32_t window = dest + byteCount - offset - 1;
                for (int j = 0; j < length; j++) {
                    writeValue |= (R8(window++) << byteShift);
                    byteShift += 8;
                    byteCount++;
                    if (byteCount == 2) {
                        W16(dest, (uint16_t)writeValue);
                        dest += 2;
                        byteCount = byteShift = 0;
                        writeValue = 0;
                        cost += 2;
                    }
                    if (--len == 0) { *cycles = cost; return 1; }
                }
            } else {
                writeValue |= (R8(source++) << byteShift);
                byteShift += 8;
                byteCount++;
                if (byteCount == 2) {
                    W16(dest, (uint16_t)writeValue);
                    dest += 2;
                    byteCount = byteShift = 0;
                    writeValue = 0;
                    cost += 2;
                }
                if (--len == 0) { *cycles = cost; return 1; }
            }
            d <<= 1;
        }
    }
    *cycles = cost;
    return 1;
}

static int hle_rl_wram(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1];
    uint32_t header = R32(source);
    int len = (int)(header >> 8);
    int cost = 24;

    source += 4;
    if (!src_ok(source, (uint32_t)len))
        return 0;

    while (len > 0) {
        uint8_t d = R8(source++);
        int l = d & 0x7F;
        if (d & 0x80) {
            uint8_t data = R8(source++);
            l += 3;
            for (int i = 0; i < l; i++) {
                W8(dest++, data);
                cost += 2;
                if (--len == 0) { *cycles = cost; return 1; }
            }
        } else {
            l++;
            for (int i = 0; i < l; i++) {
                W8(dest++, R8(source++));
                cost += 2;
                if (--len == 0) { *cycles = cost; return 1; }
            }
        }
    }
    *cycles = cost;
    return 1;
}

static int hle_rl_vram(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0] & ~3u, dest = regs[1];
    uint32_t header = R32(source);
    int len = (int)(header >> 8);
    int byteCount = 0, byteShift = 0;
    uint32_t writeValue = 0;
    int cost = 24;

    source += 4;
    if (!src_ok(source, (uint32_t)len))
        return 0;

    while (len > 0) {
        uint8_t d = R8(source++);
        int l = d & 0x7F;
        if (d & 0x80) {
            uint8_t data = R8(source++);
            l += 3;
            for (int i = 0; i < l; i++) {
                writeValue |= (data << byteShift);
                byteShift += 8;
                byteCount++;
                if (byteCount == 2) {
                    W16(dest, (uint16_t)writeValue);
                    dest += 2;
                    byteCount = byteShift = 0;
                    writeValue = 0;
                    cost += 2;
                }
                if (--len == 0) { *cycles = cost; return 1; }
            }
        } else {
            l++;
            for (int i = 0; i < l; i++) {
                writeValue |= (R8(source++) << byteShift);
                byteShift += 8;
                byteCount++;
                if (byteCount == 2) {
                    W16(dest, (uint16_t)writeValue);
                    dest += 2;
                    byteCount = byteShift = 0;
                    writeValue = 0;
                    cost += 2;
                }
                if (--len == 0) { *cycles = cost; return 1; }
            }
        }
    }
    *cycles = cost;
    return 1;
}

static int hle_huff(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1];
    uint32_t header = R32(source);
    int len = (int)(header >> 8);
    int cost = 40;
    uint8_t treeSize;
    uint32_t treeStart;
    uint32_t mask, data;
    int pos;
    uint8_t rootNode, currentNode;
    int writeData;
    int byteShift, byteCount;
    uint32_t writeValue;

    source += 4;
    if (!src_ok(source, (uint32_t)len))
        return 0;

    treeSize = R8(source++);
    treeStart = source;
    source += ((treeSize + 1u) << 1) - 1u;

    mask = 0x80000000u;
    data = R32(source);
    source += 4;
    pos = 0;
    rootNode = R8(treeStart);
    currentNode = rootNode;
    writeData = 0;
    byteShift = byteCount = 0;
    writeValue = 0;

    if ((header & 0x0F) == 8) {
        while (len > 0) {
            if (pos == 0) pos++;
            else pos += (((currentNode & 0x3F) + 1) << 1);

            if (data & mask) {
                if (currentNode & 0x40) writeData = 1;
                currentNode = R8(treeStart + pos + 1);
            } else {
                if (currentNode & 0x80) writeData = 1;
                currentNode = R8(treeStart + pos);
            }
            if (writeData) {
                writeValue |= ((uint32_t)currentNode << byteShift);
                byteCount++;
                byteShift += 8;
                pos = 0;
                currentNode = rootNode;
                writeData = 0;
                if (byteCount == 4) {
                    W32(dest, writeValue);
                    dest += 4;
                    len -= 4;
                    writeValue = 0;
                    byteCount = byteShift = 0;
                    cost += 4;
                }
            }
            mask >>= 1;
            if (mask == 0) {
                mask = 0x80000000u;
                data = R32(source);
                source += 4;
                cost += 2;
            }
        }
    } else {
        while (len > 0) {
            if (pos == 0) pos++;
            else pos += (((currentNode & 0x3F) + 1) << 1);

            if (data & mask) {
                if (currentNode & 0x40) writeData = 1;
                currentNode = R8(treeStart + pos + 1);
            } else {
                if (currentNode & 0x80) writeData = 1;
                currentNode = R8(treeStart + pos);
            }
            if (writeData) {
                writeValue |= ((uint32_t)currentNode << byteShift);
                byteCount++;
                byteShift += 8;
                pos = 0;
                currentNode = rootNode;
                writeData = 0;
                if (byteCount == 2) {
                    W16(dest, (uint16_t)writeValue);
                    dest += 2;
                    len -= 2;
                    writeValue = 0;
                    byteCount = byteShift = 0;
                    cost += 3;
                }
            }
            mask >>= 1;
            if (mask == 0) {
                mask = 0x80000000u;
                data = R32(source);
                source += 4;
                cost += 2;
            }
        }
    }
    *cycles = cost;
    return 1;
}

static int hle_diff8_wram(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1];
    uint32_t header = R32(source);
    int len = (int)(header >> 8);
    uint8_t data;
    int cost = 24;

    source += 4;
    if (!src_ok(source, (uint32_t)len))
        return 0;

    data = R8(source++);
    W8(dest++, data);
    len--;
    cost += 2;
    while (len > 0) {
        data += R8(source++);
        W8(dest++, data);
        len--;
        cost += 2;
    }
    *cycles = cost;
    return 1;
}

static int hle_diff8_vram(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1];
    uint32_t header = R32(source);
    int len = (int)(header >> 8);
    uint8_t data;
    uint16_t writeData;
    int shift = 8, bytes = 1;
    int cost = 24;

    source += 4;
    if (!src_ok(source, (uint32_t)len))
        return 0;

    data = R8(source++);
    writeData = data;
    while (len >= 2) {
        data += R8(source++);
        writeData |= (uint16_t)(data << shift);
        bytes++;
        shift += 8;
        if (bytes == 2) {
            W16(dest, writeData);
            dest += 2;
            len -= 2;
            bytes = 0;
            writeData = 0;
            shift = 0;
            cost += 3;
        }
    }
    *cycles = cost;
    return 1;
}

static int hle_diff16(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1];
    uint32_t header = R32(source);
    int len = (int)(header >> 8);
    uint16_t data;
    int cost = 24;

    source += 4;
    if (!src_ok(source, (uint32_t)len))
        return 0;

    data = R16(source);
    source += 2;
    W16(dest, data);
    dest += 2;
    len -= 2;
    cost += 3;
    while (len >= 2) {
        data += R16(source);
        source += 2;
        W16(dest, data);
        dest += 2;
        len -= 2;
        cost += 3;
    }
    *cycles = cost;
    return 1;
}

static int hle_bit_unpack(unsigned *regs, int *cycles)
{
    uint32_t source = regs[0], dest = regs[1], header = regs[2];
    int len = (int)R16(header);
    int bits, revbits, dataSize;
    uint32_t base;
    int addBase;
    int data = 0, bitwritecount = 0;
    int cost = 32;

    if (!src_ok(source, (uint32_t)len))
        return 0;

    bits = R8(header + 2);
    revbits = 8 - bits;
    base = R32(header + 4);
    addBase = (base & 0x80000000u) != 0;
    base &= 0x7fffffffu;
    dataSize = R8(header + 3);

    while (1) {
        if (--len < 0)
            break;
        {
            int mask = 0xff >> revbits;
            uint8_t b = R8(source++);
            int bitcount = 0;
            cost += 2;
            while (bitcount < 8) {
                uint32_t d = b & (uint32_t)mask;
                uint32_t temp = d >> bitcount;
                if (d || addBase)
                    temp += base;
                data |= (int)(temp << bitwritecount);
                bitwritecount += dataSize;
                if (bitwritecount >= 32) {
                    W32(dest, (uint32_t)data);
                    dest += 4;
                    data = 0;
                    bitwritecount = 0;
                    cost += 2;
                }
                mask <<= bits;
                bitcount += bits;
            }
        }
    }
    *cycles = cost;
    return 1;
}

int gba_bios_hle(unsigned number, unsigned *regs, int *cycles)
{
    *cycles = 0;

    switch (number & 0xFFu) {
    case 0x06: /* Div */
        if (regs[1] == 0)
            return 0;
        hle_div(regs, 0, cycles);
        return 1;
    case 0x07: /* DivArm */
        if (regs[0] == 0)
            return 0;
        hle_div(regs, 1, cycles);
        return 1;
    case 0x08:
        hle_sqrt(regs, cycles);
        return 1;
    case 0x09:
        hle_arctan(regs, cycles);
        return 1;
    case 0x0A:
        hle_arctan2(regs, cycles);
        return 1;
    case 0x0B:
        return hle_cpu_set(regs, cycles);
    case 0x0C:
        return hle_cpu_fast_set(regs, cycles);
    case 0x0D: /* GetBiosChecksum — official GBA value */
        regs[0] = 0xBAAE187Fu;
        *cycles = 0x4000; /* roughly: sum 16 KB of BIOS */
        return 1;
    case 0x0E:
        hle_bg_affine_set(regs, cycles);
        return 1;
    case 0x0F:
        hle_obj_affine_set(regs, cycles);
        return 1;
    case 0x10:
        return hle_bit_unpack(regs, cycles);
    case 0x11:
        return hle_lz77_wram(regs, cycles);
    case 0x12:
        return hle_lz77_vram(regs, cycles);
    case 0x13:
        return hle_huff(regs, cycles);
    case 0x14:
        return hle_rl_wram(regs, cycles);
    case 0x15:
        return hle_rl_vram(regs, cycles);
    case 0x16:
        return hle_diff8_wram(regs, cycles);
    case 0x17:
        return hle_diff8_vram(regs, cycles);
    case 0x18:
        return hle_diff16(regs, cycles);
    default:
        return 0;
    }
}
