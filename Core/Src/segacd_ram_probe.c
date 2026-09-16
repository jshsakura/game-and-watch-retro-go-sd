/*
 * segacd_ram_probe.c — phase-0 gates 2, 4 and 5: prove the all-SRAM Sega CD
 * placement LINKS on the current tree (gate 2, placeholders below) and then,
 * on the device, that the heap margin is real (gate 4) and every bank of the
 * placement — including the eight NON-CONTIGUOUS PRG pages — actually holds
 * data (gate 5, segacd_ram_probe_run_if_requested()).
 *
 * docs/SEGACD_REASSESSMENT_2026-09-14.md "Required phase-0 gate" items 2, 4
 * and 5: a linker-only target "without the emulator, containing the exact
 * static overlay plus full PRG/Word/PCM/YM allocations"; "assert at runtime
 * that at least a defined DTCM/newlib margin remains"; "exercise
 * read/write/checksum over all eight non-contiguous PRG pages on the device".
 * Every array below is a placeholder sized from the MEASURED tag
 * testbed-full-20260724-1447 map, placed in the bank the reassessment assigns
 * it, so the linker either proves the 2026-09-14 arithmetic or refuses.
 *
 * Sizes and their provenance (tag map, rebuilt 2026-09-14):
 *   resident overlay code+.data  19,112   (AXI overlay, LOAD)
 *   overlay BSS                 156,212   (AXI overlay)
 *   Word RAM                    262,144   (AXI overlay, dynamic in the port)
 *   PRG RAM pages 0..6          7x65,536  (AXI overlay, dynamic page[8])
 *   PRG RAM page 7               65,536   (DTCM heap claim at runtime — NOT
 *                                          a section here; the .ld asserts
 *                                          _heap_end-_heap_start >= 90,232)
 *   PCM RAM                      65,536   (AHB, core-specific 120K window)
 *   YM2612 tables                47,104   (AHB)
 *   CDDA mix buffer               4,800   (AHB; moved out of overlay BSS to
 *                                          close the AXI 1,244-byte gap)
 *   main 68K RAM                  65,536  (ITCM)
 *
 * The single-framebuffer premise gives the AXI overlay the span
 * 0x24025800..0x24100000 = 894,976 bytes (the second RGB565 320x240 buffer's
 * 153,600 bytes returned to RAM_EMU). When SEGACD_RAM_PROBE is off this file
 * is not compiled and every probe section in the .ld is empty and inert.
 */
#ifdef SEGACD_RAM_PROBE

#include <stdint.h>

/* ---- AXI overlay: LOAD part (tag's resident code+data), AT > CORES ------- */
static const unsigned char probe_segacd_overlay_code[19112]
    __attribute__((section(".segacd_probe_ovl"), used, aligned(4))) = {1};

/* ---- AXI overlay: BSS part (NOLOAD), chained after the LOAD end ----------
 * 156,212 minus the 4,800-byte CDDA mix buffer, which the reassessment moves
 * to AHB to close the AXI 1,244-byte gap (this is NOT double-counting: the
 * tag's overlay BSS included the CDDA buffer; here it is only in AHB). */
static unsigned char probe_segacd_static_bss[151412]
    __attribute__((section(".bss.probe_segacd_static"), used, aligned(4)));

static unsigned char probe_segacd_word_ram[262144]
    __attribute__((section(".bss.probe_segacd_word"), used, aligned(4)));

/* PRG RAM: seven 64 KiB host-backed pages. The port must expose these as
 * page[8] pointers (page 7 lives in DTCM at runtime) — a contiguous pointer
 * would recreate the 128 KiB bug the reassessment closed. */
static unsigned char probe_segacd_prg_page0[65536]
    __attribute__((section(".bss.probe_segacd_prg0"), used, aligned(4)));
static unsigned char probe_segacd_prg_page1[65536]
    __attribute__((section(".bss.probe_segacd_prg1"), used, aligned(4)));
static unsigned char probe_segacd_prg_page2[65536]
    __attribute__((section(".bss.probe_segacd_prg2"), used, aligned(4)));
static unsigned char probe_segacd_prg_page3[65536]
    __attribute__((section(".bss.probe_segacd_prg3"), used, aligned(4)));
static unsigned char probe_segacd_prg_page4[65536]
    __attribute__((section(".bss.probe_segacd_prg4"), used, aligned(4)));
static unsigned char probe_segacd_prg_page5[65536]
    __attribute__((section(".bss.probe_segacd_prg5"), used, aligned(4)));
static unsigned char probe_segacd_prg_page6[65536]
    __attribute__((section(".bss.probe_segacd_prg6"), used, aligned(4)));

/* ---- AHB: core-specific 120 KiB window at 0x30000000 (GBA addresses
 * reused under the cores-are-mutually-exclusive rule) --------------------- */
static unsigned char probe_segacd_pcm_ram[65536]
    __attribute__((section(".bss.probe_segacd_pcm"), used, aligned(4)));

static unsigned char probe_segacd_ym_tables[47104]
    __attribute__((section(".bss.probe_segacd_ym"), used, aligned(4)));

static unsigned char probe_segacd_cdda_mix[4800]
    __attribute__((section(".bss.probe_segacd_cdda"), used, aligned(4)));

/* ---- ITCM: main 68K RAM (mutually exclusive with .overlay_pce_itc and
 * .overlay_md32x_itc at the same VMA — same overlay convention) ------------ */
static unsigned char probe_segacd_main68k_ram[65536]
    __attribute__((section(".bss.probe_segacd_itcm"), used, aligned(4)));

/* Symbols for map/reporting, kept inside the overlay LOAD part so normal
 * DTCM .data is untouched. PRG page 7's DTCM claim is asserted in the .ld
 * against _heap_end-_heap_start, not by a section. */
const void *const segacd_ram_probe_pages[8]
    __attribute__((section(".segacd_probe_ovl"), used, aligned(4))) = {
    probe_segacd_prg_page0, probe_segacd_prg_page1, probe_segacd_prg_page2,
    probe_segacd_prg_page3, probe_segacd_prg_page4, probe_segacd_prg_page5,
    probe_segacd_prg_page6, (const void *)0 /* page 7: DTCM heap, runtime-claimed */
};

/* ===========================================================================
 * Runtime half — gates 4 and 5, on the device.
 *
 * Trigger follows the rc_probe convention: hold GAME+TIME during boot. The
 * probe reports on the LCD and via printf (captured in main.c's logbuf, which
 * SWD can read), then halts in a watchdog-safe loop forever.
 *
 * Gate 4 (runtime DTCM/newlib margin): PRG page 7 is a 65,536-byte malloc
 * from the DTCM heap. After that claim, one further block of
 * margin-minus-16 must allocate cleanly — a real claim, which is stronger
 * evidence than a free-space counter (fragments can be unclaimable). The
 * 16-byte allowance covers the two chunk headers + alignment: the
 * reassessment itself says "alignment, FatFs/newlib use ... must still be
 * charged against" the 33,692-byte total, and these are the first charges.
 * Deliberately NOT mallinfo(): newlib nano's __malloc_current_mallinfo is a
 * 36-byte DTCM global, and this probe adds ZERO DTCM .bss (gate 3 paid 24
 * bytes of heap for one 8-byte static before going stateless).
 *
 * Gate 5 (8 non-contiguous PRG pages): two write/read rounds of different
 * multiplicative patterns plus an FNV-1a checksum over every u32 of every
 * region in the placement. The per-region base addresses in the report are
 * the non-contiguity proof — PRG pages 0..6 are AXI (0x24xxxxxx) and page 7
 * is DTCM (0x20xxxxxx), which a contiguous pointer could never express.
 * =========================================================================== */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "main.h"             /* wdog_refresh */
#include "stm32h7xx_hal.h"    /* HAL_Delay */
#include "gw_buttons.h"       /* B_GAME, B_TIME */
#include "gw_lcd.h"           /* GW_LCD_*, lcd_backlight_off, lcd_sync */
#include "odroid_overlay.h"   /* odroid_overlay_draw_text */
#include "odroid_display.h"   /* odroid_display_set_backlight */
#include "odroid_colors.h"    /* C_GREEN, C_RED, C_WHITE */
#include "gittag.h"           /* GIT_TAG */

#define SCD_COMBO            (B_GAME | B_TIME)  /* same pair as rc_probe   */
#define SCD_PRG_BYTES        65536u
#define SCD_PRG_WORDS        (SCD_PRG_BYTES / 4u)
#define SCD_MARGIN_REQUIRED  24696u   /* reassessment: heap left after p7  */
#define SCD_MARGIN_ALLOWANCE 16u      /* page-7 chunk header + alignment   */

/* The LCD cursor is threaded explicitly (no file-scope static): the probe
 * must add ZERO DTCM .bss — the placement contract has a 4-byte heap margin
 * and gate 3 paid 24 bytes of heap for one 8-byte static before going
 * stateless. rc_probe's static rc_y is affordable there and not here. */
static uint16_t scd_line(uint16_t y, uint16_t color, const char *fmt, ...)
{
    char buf[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("scdprobe: %s\n", buf);
    if (y < GW_LCD_HEIGHT)
        y = (uint16_t)odroid_overlay_draw_text(0, y, GW_LCD_WIDTH,
                                               buf, color, C_BLACK);
    return y;
}

typedef struct {
    const char *name;
    volatile uint32_t *p;
    uint32_t words;
} scd_region_t;

/* Two write/read rounds (pattern, then its inverse with a different stride)
 * + FNV-1a over the final contents. Returns 0 on pass, 1 on any mismatch. */
static int scd_rw_region(volatile uint32_t *p, uint32_t words, uint32_t seed,
                         uint32_t *cs_out)
{
    uint32_t i, r, h = 0x811c9dc5u;
    for (r = 0; r < 2; r++) {
        uint32_t base = r ? ~seed : seed;
        uint32_t step = r ? 40503u : 2654435761u;
        for (i = 0; i < words; i++) p[i] = base + i * step;
        for (i = 0; i < words; i++)
            if (p[i] != base + i * step) return 1;
    }
    for (i = 0; i < words; i++) { h ^= p[i]; h *= 0x01000193u; }
    *cs_out = h;
    return 0;
}

void segacd_ram_probe_run_if_requested(uint32_t boot_buttons)
{
    unsigned char *p7;
    uint16_t y = 0;
    scd_region_t regs[13];
    uint32_t i, ok = 0, cs;
    int fail, g4_pass;
    void *m;

    if ((boot_buttons & SCD_COMBO) != SCD_COMBO) return;   /* inert otherwise */

    lcd_backlight_off();
    odroid_display_set_backlight(ODROID_BACKLIGHT_LEVEL6);
    y = scd_line(y, C_GREEN, "SCD PROBE %s", GIT_TAG);

    /* ---- gate 4: DTCM heap serves page 7 + the newlib margin ------------- */
    p7 = (unsigned char *)malloc(SCD_PRG_BYTES);
    if (!p7) {
        y = scd_line(y, C_RED, "G4 FAIL: malloc(65536) page7");
        g4_pass = 0;
    } else {
        /* The margin itself, claimed for real: margin-minus-allowance must
         * allocate cleanly AFTER page 7, then it is freed — page 7 stays
         * claimed for the gate-5 test below. */
        m = malloc(SCD_MARGIN_REQUIRED - SCD_MARGIN_ALLOWANCE);
        g4_pass = (m != NULL);
        y = scd_line(y, g4_pass ? C_GREEN : C_RED,
                     "G4 page7@%p margin claim %s",
                     (void *)p7, m ? "ok" : "FAIL");
        free(m);
    }
    printf("scdprobe: G4 %s (page7=%p)\n",
           g4_pass ? "PASS" : "FAIL", (void *)p7);

    /* ---- gate 5: R/W + checksum every region of the placement ------------ */
    regs[0]  = (scd_region_t){"prg0",  (volatile uint32_t *)probe_segacd_prg_page0, SCD_PRG_WORDS};
    regs[1]  = (scd_region_t){"prg1",  (volatile uint32_t *)probe_segacd_prg_page1, SCD_PRG_WORDS};
    regs[2]  = (scd_region_t){"prg2",  (volatile uint32_t *)probe_segacd_prg_page2, SCD_PRG_WORDS};
    regs[3]  = (scd_region_t){"prg3",  (volatile uint32_t *)probe_segacd_prg_page3, SCD_PRG_WORDS};
    regs[4]  = (scd_region_t){"prg4",  (volatile uint32_t *)probe_segacd_prg_page4, SCD_PRG_WORDS};
    regs[5]  = (scd_region_t){"prg5",  (volatile uint32_t *)probe_segacd_prg_page5, SCD_PRG_WORDS};
    regs[6]  = (scd_region_t){"prg6",  (volatile uint32_t *)probe_segacd_prg_page6, SCD_PRG_WORDS};
    regs[7]  = (scd_region_t){"prg7",  (volatile uint32_t *)p7,                    SCD_PRG_WORDS}; /* DTCM heap */
    regs[8]  = (scd_region_t){"m68k",  (volatile uint32_t *)probe_segacd_main68k_ram, 16384};      /* ITCM */
    regs[9]  = (scd_region_t){"word",  (volatile uint32_t *)probe_segacd_word_ram,  65536};        /* AXI */
    regs[10] = (scd_region_t){"static",(volatile uint32_t *)probe_segacd_static_bss, 37853};       /* AXI */
    regs[11] = (scd_region_t){"pcm",   (volatile uint32_t *)probe_segacd_pcm_ram,   16384};        /* AHB */
    regs[12] = (scd_region_t){"ym",    (volatile uint32_t *)probe_segacd_ym_tables, 11776};        /* AHB */

    for (i = 0; i < 13; i++) {
        cs = 0;
        fail = scd_rw_region(regs[i].p, regs[i].words, 0x53454741u + i, &cs);
        if (!fail) ok++;
        y = scd_line(y, fail ? C_RED : C_GREEN, "%-6s %p cs=%08lx %s",
                     regs[i].name, (void *)regs[i].p, (unsigned long)cs,
                     fail ? "RW FAIL" : "ok");
    }
    /* CDDA is 4,800 B = 1,200 words, too small for the u32 stride pattern to
     * be interesting, but it is still part of the AHB window — cover it. */
    fail = scd_rw_region((volatile uint32_t *)probe_segacd_cdda_mix, 1200,
                         0x43444441u, &cs);
    if (!fail) ok++;
    y = scd_line(y, fail ? C_RED : C_GREEN, "%-6s %p cs=%08lx %s",
                 "cdda", (void *)probe_segacd_cdda_mix, (unsigned long)cs,
                 fail ? "RW FAIL" : "ok");

    y = scd_line(y, (g4_pass && ok == 14) ? C_GREEN : C_RED,
                 "G4 %s  G5 %lu/14", g4_pass ? "PASS" : "FAIL",
                 (unsigned long)ok);
    printf("scdprobe: prg6=%p (AXI) prg7=%p (DTCM) -- pages non-contiguous\n",
           (void *)probe_segacd_prg_page6, (void *)p7);
    lcd_sync();

    printf("scdprobe: halt (probe done)\n");
    for (;;) { wdog_refresh(); HAL_Delay(50); }
}

#endif /* SEGACD_RAM_PROBE */
