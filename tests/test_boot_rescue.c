/* The boot-loop rescue counter, against a fake backup register.
 *
 * This compiles the REAL Core/Src/gw_boot_rescue.c. The register is one
 * uint32_t that survives "resets" (the test resetting the session statics)
 * exactly like the RTC backup register survives a watchdog reset, and gets
 * scribbled on to play the part of a wiped backup domain.
 *
 * What must hold:
 *  - garbage in the register reads as zero failures, never as a boot loop;
 *  - two boots that never mark themselves alive make the third boot stop;
 *  - "alive" needs both the poll count and the uptime, not just one;
 *  - a clean shutdown wipes the count, so on/off cycling is not a boot loop.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "gw_boot_rescue.h"
#include "boot_rescue_stubs.h"

static uint32_t fake_bkp;
static uint32_t fake_ms;

uint32_t rescue_bkp_read(void) { return fake_bkp; }
void rescue_bkp_write(uint32_t value) { fake_bkp = value; }
uint32_t rescue_now_ms(void) { return fake_ms; }

static int failures = 0;

static void ok(bool cond, const char *what)
{
    printf("  %s %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) failures++;
}

/* One power-on: statics reset (a real reset clears RAM), register kept. */
static void reboot(void)
{
    boot_rescue_test_reset_session();
    fake_ms = 0;
    boot_rescue_note_boot_start();
}

static void run_alive(uint32_t polls, uint32_t at_ms)
{
    fake_ms = at_ms;
    for (uint32_t i = 0; i < polls; i++) {
        boot_rescue_mark_alive_tick();
    }
}

int main(void)
{
    /* A wiped or never-written backup domain must read as "no failures". */
    fake_bkp = 0xDEADBEEF;
    reboot();
    ok(!boot_rescue_screen_due(), "garbage register is not a boot loop");
    ok((fake_bkp & 0xFFFF0000u) == 0xB0070000u, "first boot stamps the magic");
    ok((fake_bkp & 0xFFFFu) == 1, "first boot is attempt 1");

    /* Boot 2: the first one never marked alive. Still not due. */
    reboot();
    ok(!boot_rescue_screen_due(), "one failed boot does not trip the screen");

    /* Boot 3: two failures behind us. Now the screen is due. */
    reboot();
    ok(boot_rescue_screen_due(), "two failed boots trip the rescue screen");
    ok(boot_rescue_test_failed_boots() == 2, "the screen knows the count");

    /* Alive needs BOTH signals: polls without uptime is a fast crash loop,
     * uptime without polls is a hang with interrupts still ticking. */
    reboot();
    run_alive(1000, 500);
    ok((fake_bkp & 0xFFFFu) != 0, "1000 polls in 0.5s do not clear the count");
    boot_rescue_test_reset_session();
    fake_bkp = 0xB0070000u;
    boot_rescue_note_boot_start();
    run_alive(3, 60000);
    ok((fake_bkp & 0xFFFFu) != 0, "3 polls at 60s do not clear the count");

    /* Enough of both: the counter clears and stays cleared. */
    run_alive(400, 60001);
    ok((fake_bkp & 0xFFFFu) == 0, "sustained polling clears the count");
    reboot();
    ok(!boot_rescue_screen_due(), "a healthy boot resets the streak");

    /* Deliberate power-off mid-boot is not a failure. */
    reboot();
    reboot();
    boot_rescue_mark_clean_shutdown();
    reboot();
    ok(!boot_rescue_screen_due(), "clean shutdown wipes the streak");

    /* Nobody chose launcher-only boot unless the screen said so. */
    ok(!boot_rescue_force_launcher(), "no forced launcher without the screen");

    printf(failures ? "test_boot_rescue: %d FAILED\n"
                    : "test_boot_rescue: all passed\n",
           failures);
    return failures ? 1 : 0;
}
