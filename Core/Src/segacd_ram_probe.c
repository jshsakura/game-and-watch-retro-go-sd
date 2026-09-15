/*
 * segacd_ram_probe.c — phase-0 gate 2: prove the all-SRAM Sega CD placement
 * LINKS on the current tree, before any emulator code is ported forward.
 *
 * docs/SEGACD_REASSESSMENT_2026-09-14.md "Required phase-0 gate" item 2:
 * a linker-only target "without the emulator, containing the exact static
 * overlay plus full PRG/Word/PCM/YM allocations". Every array below is a
 * placeholder sized from the MEASURED tag testbed-full-20260724-1447 map,
 * placed in the bank the reassessment assigns it, so the linker either
 * proves the 2026-09-14 arithmetic or refuses.
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

#endif /* SEGACD_RAM_PROBE */
