/* Does load_gamepak() survive an XIP cart?
 *
 * The device runs gpSP with ROM_BUFFER_SIZE=0: the cart is never copied into RAM,
 * it stays memory-mapped in QSPI flash and gba_set_xip_rom() hands the core a
 * pointer to it. load_gamepak_raw() then takes its XIP branch and returns BEFORE
 * it allocates a single gamepak_buffer — so `gamepak_buffers[0]` is NULL for the
 * whole run, and anything that reads the cart through it reads address 0.
 *
 * That is what killed Pokemon Ruby on hardware: load_gamepak()'s save-type
 * detection scanned 1 MB "of the ROM" starting at 0x00000000. On the device the
 * first 64 KB of that is ITCM — readable — and the byte after it is not, so the
 * scan ran a while and then took a bus fault inside memcmp. The QEMU harness
 * never saw it: on an mps2-an500 address 0 is mapped, so the same scan quietly
 * read a megabyte of nothing and moved on.
 *
 * This harness is a host build precisely because a host build DOES trap it —
 * page zero is unmapped on any hosted OS, so the bug is a SIGSEGV rather than a
 * silent lie. Same lesson, and the same shape, as tools/sm_harness/device_run.sh:
 * a harness only proves something if it is the same program the device runs, on a
 * machine with the device's rules.
 *
 * RED:   git stash / check out gba_memory.c from before the fix -> SIGSEGV
 * GREEN: with gamepak_header() used throughout the load path -> exits 0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint32_t u32;

void  init_memory(void);
void  init_main(void);
void  gba_set_xip_rom(u8 *base, u32 size);
void  init_gamepak_buffer(void);
u32   load_gamepak(const void *info, const char *name, int rtc, int rumble, int serial);

/* A 16 MB Ruby: only the header matters to the code under test. The Korean fan
 * translation is what the device was running, so use its game code — AXVK — which
 * gba_over.h knows, and which sets FLAGS_FLASH_128KB but NOT backup_type_reset.
 * That is why the save-type scan runs at all. */
#define ROM_SIZE (16 * 1024 * 1024)

/* The device's watchdog, stubbed so we can count it.
 *
 * This counts the WHOLE chain, which is the point: gpSP's save-type scan calls
 * gba_scan_yield(), gba_frontend.c overrides that weak no-op with wdog_refresh(),
 * and wdog_refresh() is what keeps the machine alive. The scan reads a megabyte of
 * cart — memory-mapped QSPI flash on the device — which is many times the ~472 ms
 * watchdog window. Without the kick the machine resets in the middle of it and the
 * player lands back on the game list: no fault, no BSOD, no message, the emulator
 * simply never appears to start.
 *
 * A unit test of wdog_refresh() could never have caught that, because
 * wdog_refresh() was never broken. What was missing was the CALL. So count calls. */
static unsigned g_wdog_kicks;
void wdog_refresh(void) { g_wdog_kicks++; }

int main(void)
{
    u8 *rom = calloc(1, ROM_SIZE);
    if (!rom)
        return 77;

    memcpy(&rom[0xA0], "POKEMON RUBY", 12);   /* title      */
    memcpy(&rom[0xAC], "AXVK", 4);            /* game code  */
    memcpy(&rom[0xB0], "01", 2);              /* maker code */
    rom[0xB2] = 0x96;                         /* fixed value */

    init_main();
    init_memory();

    gba_set_xip_rom(rom, ROM_SIZE);
    init_gamepak_buffer();

    printf("harness: load_gamepak() on a %d MB XIP cart...\n", ROM_SIZE / (1024 * 1024));
    fflush(stdout);

    if (load_gamepak(NULL, "ruby.gba", 0, 0, 0) != 0) {
        printf("FAIL: load_gamepak() rejected the cart\n");
        return 1;
    }

    printf("harness: the cart scan kicked the watchdog %u times\n", g_wdog_kicks);
    if (g_wdog_kicks == 0) {
        printf("FAIL: the save-type scan never kicked the watchdog.\n");
        printf("      On the device that is a watchdog reset in the middle of the\n");
        printf("      scan: back to the game list, no fault, nothing on screen.\n");
        return 1;
    }

    printf("PASS: load_gamepak() read the cart through the XIP pointer, not NULL,\n");
    printf("      and kicked the watchdog while it scanned it\n");
    return 0;
}
