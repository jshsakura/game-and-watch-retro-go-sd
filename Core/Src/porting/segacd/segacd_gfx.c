/* Sega/Mega CD — Word-RAM graphics-transform ASIC (2M-mode rotation/scaling).
 *
 * Ported from Genesis Plus GX (pd_cd/gfx.c, Copyright (C) 2012 Eke-Eke), whose
 * license permits non-commercial redistribution of derivative works with the
 * source. The pixel pipeline (gfx_pixel + the RENDER_LOOP macro + the three
 * lookup tables) is kept byte-for-byte faithful; only the environment bindings
 * change:
 *   - Pico_mcd->word_ram2M      -> SCD.word_ram          (same ^1 byte layout)
 *   - Pico_mcd->s68k_regs[N]    -> SCD.s68k_regs[N]
 *   - pcd_irq_s68k(1,1)         -> SCD.gfx_int_pending    (frame-paced level-1)
 *   - the cycle-scheduled multi-step gfx_update           -> a single-shot render
 *
 * The single-shot model is deliberate: our engine has no s68k event scheduler,
 * and the BIOS boot logo only needs the transformed image to exist in Word-RAM
 * before the sub's next frame + a completion INT1. So a $FF8066 (start) write
 * renders ALL lines immediately, clears GRON ($FF8058 bit7) and the line
 * counter ($FF8064/65), and arms the level-1 completion interrupt (delivered
 * frame-paced by segacd_run_sub, gated on IEN1 = $FF8033 bit1).
 *
 * Byte order: Word-RAM is stored in the gwenesis ^1-swapped layout (a byte at
 * logical address A lives at word_ram[A^1]); a uint16* read of an even offset
 * on a little-endian host therefore yields the logical big-endian word, exactly
 * as GPGX's word_ram2M does — so tracePtr/mapPtr and READ_BYTE/WRITE_BYTE port
 * verbatim. On the device (big-endian data? no — Cortex-M7 is little-endian)
 * the same holds.
 */
#include <string.h>
#include "segacd.h"

/* Word-RAM byte access — the ^1 convention (matches gwenesis macros.h and
 * GPGX's MEM_BE2 on a little-endian target). */
#define WR_RD_BYTE(BASE, ADDR)      ((BASE)[(ADDR) ^ 1])
#define WR_WR_BYTE(BASE, ADDR, VAL) ((BASE)[(ADDR) ^ 1] = (uint8_t)(VAL))

typedef struct {
    uint32_t dotMask;                 /* stamp map size mask */
    uint32_t stampMask;               /* stamp number mask */
    uint16_t *tracePtr;               /* trace vector pointer (into Word-RAM) */
    uint16_t *mapPtr;                 /* stamp map table base (into Word-RAM) */
    uint8_t  stampShift;              /* stamp pixel shift (stamp size) */
    uint8_t  mapShift;                /* stamp map table shift (map size) */
    uint16_t bufferOffset;            /* image buffer column offset */
    uint32_t bufferStart;             /* image buffer start index */
    uint8_t  lut_prio[4][0x10][0x10]; /* Word-RAM write-priority LUT */
    uint8_t  lut_pixel[0x200];        /* dot offset LUT */
    uint16_t lut_cell2[0x80];         /* stamp offset LUT (16x16) */
    uint16_t lut_cell4[0x80];         /* stamp offset LUT (32x32) */
} segacd_gfx_t;

static segacd_gfx_t gfx;

void segacd_gfx_init(void)
{
    int i, j;
    uint8_t row, col, temp;

    memset(&gfx, 0, sizeof(gfx));

    /* priority modes */
    for (i = 0; i < 0x10; i++) {
        for (j = 0; j < 0x10; j++) {
            gfx.lut_prio[0][i][j] = (uint8_t)j;              /* normal */
            gfx.lut_prio[1][i][j] = (uint8_t)(i ? i : j);    /* underwrite */
            gfx.lut_prio[2][i][j] = (uint8_t)(j ? j : i);    /* overwrite */
            gfx.lut_prio[3][i][j] = (uint8_t)i;              /* invalid */
        }
    }

    /* cell LUT: entry = yyxxhrr (7 bits) */
    for (i = 0; i < 0x80; i++) {
        row = (uint8_t)((i >> 5) & 3);
        col = (uint8_t)((i >> 3) & 3);
        if (i & 4) { col = (uint8_t)(col ^ 3); }
        if (i & 2) { col = (uint8_t)(col ^ 3); row = (uint8_t)(row ^ 3); }
        if (i & 1) { temp = col; col = (uint8_t)(row ^ 3); row = temp; }
        gfx.lut_cell2[i] = (uint16_t)(((row & 1) + (col & 1) * 2) << 6);
        gfx.lut_cell4[i] = (uint16_t)(((row & 3) + (col & 3) * 4) << 6);
    }

    /* pixel LUT: entry = yyyxxxhrr (9 bits) */
    for (i = 0; i < 0x200; i++) {
        row = (uint8_t)((i >> 6) & 7);
        col = (uint8_t)((i >> 3) & 7);
        if (i & 4) { col = (uint8_t)(col ^ 7); }
        if (i & 2) { col = (uint8_t)(col ^ 7); row = (uint8_t)(row ^ 7); }
        if (i & 1) { temp = col; col = (uint8_t)(row ^ 7); row = temp; }
        gfx.lut_pixel[i] = (uint8_t)(col + row * 8);
    }
}

static inline int gfx_pixel(uint32_t xpos, uint32_t ypos, uint16_t *lut_cell)
{
    uint16_t stamp_data;
    uint32_t stamp_index;
    uint8_t  pixel_out = 0x00;

    if (((xpos | ypos) & ~gfx.dotMask) == 0) {
        stamp_data = gfx.mapPtr[(xpos >> gfx.stampShift) |
                                ((ypos >> gfx.stampShift) << gfx.mapShift)];
        stamp_index = (uint32_t)(stamp_data & gfx.stampMask) << 8;
        if (stamp_index) {
            stamp_data = (uint16_t)((stamp_data >> 13) & 7);
            stamp_index |= lut_cell[stamp_data | ((ypos >> 9) & 0x60) | ((xpos >> 11) & 0x18)];
            stamp_index |= gfx.lut_pixel[stamp_data | ((ypos >> 5) & 0x1c0) | ((xpos >> 8) & 0x38)];
            pixel_out = WR_RD_BYTE(SCD.word_ram, stamp_index >> 1);
            pixel_out = (uint8_t)(pixel_out >> (4 * !(stamp_index & 1)));
            pixel_out &= 0x0f;
        }
    }
    return pixel_out;
}

#define RENDER_LOOP(N, UPDP, COND1, COND2) do {                             \
    if (bufferIndex & 1) { bufferIndex ^= 1; goto right##N; }               \
    while (width--) {                                                       \
        xpos &= mask; ypos &= mask;                                         \
        if (COND1) { pixel_out = (uint8_t)gfx_pixel(xpos, ypos, lut_cell); UPDP; } \
        if (COND2) {                                                        \
            pixel_in = WR_RD_BYTE(SCD.word_ram, bufferIndex >> 1);          \
            pixel_in = (uint8_t)((lut_prio[(pixel_in & 0xf0) >> 4][pixel_out] << 4) | (pixel_in & 0x0f)); \
            WR_WR_BYTE(SCD.word_ram, bufferIndex >> 1, pixel_in);           \
        }                                                                   \
        xpos += xoffset; ypos += yoffset;                                   \
right##N:                                                                   \
        if (width-- == 0) break;                                           \
        xpos &= mask; ypos &= mask;                                         \
        if (COND1) { pixel_out = (uint8_t)gfx_pixel(xpos, ypos, lut_cell); UPDP; } \
        if (COND2) {                                                        \
            pixel_in = WR_RD_BYTE(SCD.word_ram, bufferIndex >> 1);          \
            pixel_in = (uint8_t)((lut_prio[pixel_in & 0x0f][pixel_out]) | (pixel_in & 0xf0)); \
            WR_WR_BYTE(SCD.word_ram, bufferIndex >> 1, pixel_in);           \
        }                                                                   \
        xpos += xoffset; ypos += yoffset;                                   \
        bufferIndex += 2;                                                   \
        if ((bufferIndex & 7) == 0) bufferIndex += gfx.bufferOffset - 1;    \
    }                                                                       \
} while (0)

static void gfx_render(uint32_t bufferIndex, uint32_t width)
{
    uint8_t pixel_in, pixel_out;
    uint32_t priority;
    uint8_t (*lut_prio)[0x10];
    uint16_t *lut_cell;
    uint32_t mask;

    /* pixel-map start position for this line (13.3 -> 13.11) */
    uint32_t xpos = (uint32_t)(*gfx.tracePtr++) << 8;
    uint32_t ypos = (uint32_t)(*gfx.tracePtr++) << 8;
    /* per-line offsets (5.11, signed) */
    uint32_t xoffset = (uint32_t)(int16_t)(*gfx.tracePtr++);
    uint32_t yoffset = (uint32_t)(int16_t)(*gfx.tracePtr++);

    priority = ((uint32_t)SCD.s68k_regs[2] << 8) | SCD.s68k_regs[3];
    priority = (priority >> 3) & 0x03;
    lut_prio = gfx.lut_prio[priority];

    lut_cell = (SCD.s68k_regs[0x59] & 0x02) ? gfx.lut_cell4 : gfx.lut_cell2;

    mask = 0xffffff;
    if (SCD.s68k_regs[0x59] & 0x01)
        mask = gfx.dotMask;

    pixel_out = 0;
    if (xoffset + (1U << 10) <= 1U << 11 && yoffset + (1U << 10) <= 1U << 11) {
        uint32_t oldx, oldy;
        oldx = oldy = ~xpos;
        RENDER_LOOP(1, oldx = xpos; oldy = ypos,
                    (oldx ^ xpos ^ oldy ^ ypos) >> 11, (!priority) | pixel_out);
    } else {
        RENDER_LOOP(3, , 1, (!priority) | pixel_out);
    }
}

/* $FF8066 (trace-vector base / START) write: set up the operation from the GA
 * registers and render the whole image in one shot, then flag completion.
 * Mirrors gfx_start() + gfx_update()'s render loop, collapsed. Returns 1 if an
 * op actually started (so the caller can arm the completion INT1). */
int segacd_gfx_start(uint32_t base)
{
    uint32_t mask = 0, reg;
    int w, h;

    /* only valid in 2M mode ($FF8003 bit2 == 0) */
    if (SCD.s68k_regs[3] & 0x04)
        return 0;

    gfx.tracePtr = (uint16_t *)(SCD.word_ram + ((base << 2) & 0x3fff8));

    switch ((SCD.s68k_regs[0x59] >> 1) & 0x03) {
    case 0: gfx.dotMask=0x07ffff; gfx.stampMask=0x7ff; gfx.stampShift=11+4; gfx.mapShift=4; mask=0x3fe00; break;
    case 1: gfx.dotMask=0x07ffff; gfx.stampMask=0x7fc; gfx.stampShift=11+5; gfx.mapShift=3; mask=0x3ff80; break;
    case 2: gfx.dotMask=0x7fffff; gfx.stampMask=0x7ff; gfx.stampShift=11+4; gfx.mapShift=8; mask=0x20000; break;
    case 3: gfx.dotMask=0x7fffff; gfx.stampMask=0x7fc; gfx.stampShift=11+5; gfx.mapShift=7; mask=0x38000; break;
    }

    reg = ((uint32_t)SCD.s68k_regs[0x5a] << 8) | SCD.s68k_regs[0x5b];
    gfx.mapPtr = (uint16_t *)(SCD.word_ram + ((reg << 2) & mask));

    gfx.bufferOffset = (uint16_t)((((SCD.s68k_regs[0x5d] & 0x1f) + 1) << 6) - 7);

    reg = ((uint32_t)SCD.s68k_regs[0x5e] << 8) | SCD.s68k_regs[0x5f];
    gfx.bufferStart = (reg << 3) & 0x7ffc0;
    gfx.bufferStart += (SCD.s68k_regs[0x61] & 0x3f);

    /* GRON = busy (reported until the op "completes" below) */
    SCD.s68k_regs[0x58] = 0x80;

    w = ((int)SCD.s68k_regs[0x62] << 8) | SCD.s68k_regs[0x63];
    h = ((int)SCD.s68k_regs[0x64] << 8) | SCD.s68k_regs[0x65];
#ifdef SEGACD_GA_TRACE
    { extern uint32_t scd_dbg_gfx_ops, scd_dbg_gfx_lines, scd_dbg_gfx_mapnz;
      scd_dbg_gfx_ops++; scd_dbg_gfx_lines += (h > 0 ? (uint32_t)h : 0);
      /* how much of the stamp map is non-empty (i.e. are stamps loaded)? */
      { long nz=0; for (int k=0;k<512;k++) if (((uint8_t*)gfx.mapPtr)[k]) nz++;
        if ((uint32_t)nz > scd_dbg_gfx_mapnz) scd_dbg_gfx_mapnz = (uint32_t)nz; } }
#endif

    /* render every line right now (single-shot). Guard the trace pointer
     * against running past Word-RAM (4 words/line consumed); the image buffer
     * and stamp accesses are already mask-bounded inside gfx_render/gfx_pixel. */
    {
        uint16_t *trace_end = (uint16_t *)(SCD.word_ram + SEGACD_WORD_RAM_SIZE);
        while (h-- > 0 && gfx.tracePtr + 4 <= trace_end) {
            gfx_render(gfx.bufferStart, (uint32_t)w);
            gfx.bufferStart += 8;   /* 8 pixels/line */
        }
    }

    /* completion: clear GRON + line counter */
    SCD.s68k_regs[0x58] = 0;
    SCD.s68k_regs[0x64] = 0;
    SCD.s68k_regs[0x65] = 0;
    return 1;
}

#ifdef SEGACD_GA_TRACE
uint32_t scd_dbg_gfx_ops, scd_dbg_gfx_lines, scd_dbg_gfx_mapnz;
#endif
