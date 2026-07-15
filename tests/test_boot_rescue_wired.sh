#!/bin/bash
# The power button is a GPIO the firmware reads: when the firmware hangs, the
# only way off is a flat battery. The rescue counter, the rescue screen, the
# BSOD power-off and the early watchdog only help if every one of them is
# actually CALLED — and each call sits in a different file. This pins the
# wiring, the same way test_idle_timeout_wired.sh pins the idle rule.
set -u
rc=0

need() {   # $1 = file, $2 = pattern, $3 = what the wiring is
    if grep -q "$2" "$1"; then
        echo "  OK   $3"
    else
        echo "  FAIL $3 -- '$2' not found in $1"
        rc=1
    fi
}

MAIN=Core/Src/main.c

need $MAIN "boot_rescue_note_boot_start" \
    "every boot counts itself before anything can hang"
need $MAIN "boot_rescue_screen_due"      \
    "a failed-boot streak stops at the rescue screen, pre-SD, pre-resume"
need $MAIN "boot_rescue_power_off_now"   \
    "POWER on the BSOD really powers off"
need Core/Src/retro-go/rg_main.c "boot_rescue_force_launcher" \
    "the rescue screen's 'boot to menu' skips the game auto-resume"
need Core/Src/porting/odroid_input.c "boot_rescue_mark_alive_tick" \
    "the shared input poll is the boot-succeeded signal"
need Core/Src/gw_sleep.c "boot_rescue_mark_clean_shutdown" \
    "a deliberate sleep is not a failed boot"
need Makefile "gw_boot_rescue.c" \
    "the rescue module is in the build"

# The watchdog must be armed for EVERY boot before bring-up starts, not only
# for hot boots: an unconditional wdog_enable() must appear before MPU_Config()
# in main() (the hot-boot one inside the boot_magic switch does not count,
# which is why this checks textual order against MPU_Config, the first
# bring-up call).
if awk '/^  wdog_enable\(\);/ { armed=NR } /MPU_Config\(\);/ { if (armed) found=1 }
        END { exit found ? 0 : 1 }' $MAIN; then
    echo "  OK   the watchdog is armed unconditionally before bring-up"
else
    echo "  FAIL no unconditional wdog_enable() before MPU_Config() in main()"
    rc=1
fi

exit $rc
