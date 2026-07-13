/* The two globals main_sm.c defines for the core on the device. Same values, so
 * the PPU under test is configured the way the firmware configures it — a test
 * that builds a DIFFERENT program proves nothing (root CLAUDE.md). */
#include <stdbool.h>
#include <stdint.h>

bool g_new_ppu = true;   /* main_sm.c:71 (ppu.c defines g_ppu_line_cb itself) */
