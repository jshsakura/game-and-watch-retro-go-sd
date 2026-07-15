#ifndef _GW_BOOT_RESCUE_H_
#define _GW_BOOT_RESCUE_H_

#include <stdbool.h>
#include <stdint.h>

/* Boot-loop rescue.
 *
 * The power button is a GPIO the firmware reads: once the firmware hangs, the
 * code that turns the device off is hung with it, and the unit sits dark until
 * the battery dies. These hooks make every hang end somewhere a person can
 * act:
 *
 *  - a consecutive-failed-boot counter in an RTC backup register (survives
 *    watchdog and NVIC resets, wiped only by full power loss). Two failed
 *    boots in a row and the next boot stops at a minimal rescue screen —
 *    before the SD card, the config, or any game resume is touched — offering
 *    launcher-only boot, normal boot, or power-off, and powering itself off
 *    if nobody answers.
 *  - a "boot succeeded" mark fed from the shared input poll: only a firmware
 *    that has been alive and polling input for a while clears the counter.
 *  - a minimal power-off that works from fault context (BSOD).
 */

/* Call once per boot, after MX_RTC_Init(), before anything that can hang. */
void boot_rescue_note_boot_start(void);

/* True when enough consecutive boots failed that the rescue screen must run. */
bool boot_rescue_screen_due(void);

/* Interactive rescue screen. Needs the LCD up. May not return (power off). */
void boot_rescue_screen_show(void);

/* True when the user chose "boot to launcher" — skip the game auto-resume. */
bool boot_rescue_force_launcher(void);

/* Fed from odroid_input_read_gamepad(); clears the counter once the firmware
 * has demonstrably been alive (time + poll count), not merely started. */
void boot_rescue_mark_alive_tick(void);

/* A deliberate sleep/power-off is not a failed boot: clear the counter. */
void boot_rescue_mark_clean_shutdown(void);

/* Minimal standby entry: wakeup pin armed, no HAL tick, no SD, no audio.
 * Safe from fault context with IRQs disabled. Never returns. */
void boot_rescue_power_off_now(void) __attribute__((noreturn));

#endif
