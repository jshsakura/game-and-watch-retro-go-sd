#!/bin/bash
# The Sega CD port's contracts are calls in the right place, not functions
# with testable bodies, so pin the wiring (the shape of test_idle_timeout_wired).
# Each line below is something that was missing or wrong on 2026-09-30:
#   - the device never enabled the rotation/scaling ASIC while the host rig did,
#     so every ASIC scene rendered on the host and not on the console;
#   - BRAM (the games' own save file) was formatted blank at every power-on and
#     never written anywhere;
#   - the pad ignored the MD keymap, so the in-game Controls dialog did nothing.
set -u
rc=0
M=${SEGACD_MAIN:-Core/Src/porting/segacd/main_segacd.c}
H=tools/pico_host_segacd/host_mcd.c

ok()   { echo "  OK   $1"; }
bad()  { echo "  FAIL $1"; rc=1; }
has()  { grep -q -- "$2" "$1"; }

# Only the body of app_main_segacd: line order elsewhere is not execution order.
main_body=$(sed -n '/^void app_main_segacd(/,/^}/p' "$M")
load_body=$(sed -n '/^static bool segacd_load(/,/^}/p' "$M")

# One option set for both sides.
if has "$M" "PicoIn.opt = SEGACD_PICO_OPT;" && has "$H" "PicoIn.opt = SEGACD_PICO_OPT;"; then
    ok "device and host rig read the same PicoDrive options (segacd_config.h)"
else
    bad "device and host rig configure PicoDrive separately again"
fi
if grep -q "POPT_EN_MCD_GFX" Core/Inc/porting/segacd/segacd_config.h; then
    ok "the Word-RAM ASIC renders"
else
    bad "SEGACD_PICO_OPT lost POPT_EN_MCD_GFX: ASIC scenes draw nothing on device"
fi

# BRAM: loaded after the media powers the MCD (which formats it), saved by hook.
media_line=$(echo "$main_body" | grep -n "PicoLoadMedia(" | head -1 | cut -d: -f1)
bram_line=$(echo "$main_body" | grep -n "segacd_bram_load();" | head -1 | cut -d: -f1)
if [ -n "$media_line" ] && [ -n "$bram_line" ] && [ "$bram_line" -gt "$media_line" ]; then
    ok "BRAM is loaded after PicoLoadMedia formats it"
else
    bad "BRAM is not loaded, or is loaded before PicoLoadMedia wipes it"
fi
if echo "$main_body" | grep -q "segacd_sleep_wake, segacd_bram_save,"; then
    ok "BRAM is flushed from the shutdown/sleep sram hook"
else
    bad "odroid_system_emu_init gets no sram_save callback: BRAM dies with the power"
fi
if echo "$main_body" | grep -q "segacd_bram_save();"; then
    ok "BRAM is written back while playing"
else
    bad "the frame loop never writes BRAM back"
fi
# A savestate carries BRAM; loading one must not roll the game's saves back.
save_at=$(echo "$load_body" | grep -n "segacd_bram_save();" | head -1 | cut -d: -f1)
pico_at=$(echo "$load_body" | grep -n "PicoStateFP(" | head -1 | cut -d: -f1)
reload_at=$(echo "$load_body" | grep -n "segacd_bram_load();" | head -1 | cut -d: -f1)
if [ -n "$save_at" ] && [ -n "$pico_at" ] && [ -n "$reload_at" ] \
   && [ "$save_at" -lt "$pico_at" ] && [ "$reload_at" -gt "$pico_at" ]; then
    ok "a state load keeps the current BRAM (flush before, reload after)"
else
    bad "segacd_load lets a savestate overwrite BRAM"
fi

# Input: the MD keymap drives every Genesis button, 6-button pad enabled.
n=$(grep -c "odroid_keymap_pressed(j, ODROID_KEYMAP_MD_" "$M")
if [ "$n" -eq 8 ]; then
    ok "all 8 Genesis buttons come from the remappable MD keymap"
else
    bad "only $n of 8 Genesis buttons come from the keymap"
fi
if echo "$main_body" | grep -q "PicoSetInputDevice(0, PICO_INPUT_PAD_6BTN)"; then
    ok "the 6-button pad is plugged in"
else
    bad "port 0 is not a 6-button pad"
fi

# Boot: disc-region BIOS and START through the JP/EU BIOS menu, on both sides.
if echo "$main_body" | grep -q "segacd_boot_start_pad(&boot_start, SekPc)" \
   && has "$H" "segacd_boot_start_pad(&boot, SekPc)"; then
    ok "device and host rig press START through the BIOS menu with one rule"
else
    bad "segacd_boot_start is not wired on both sides"
fi
if [ -e tools/segacd_fix_security.sh ]; then
    bad "tools/segacd_fix_security.sh is back: it erased JP discs' boot code"
else
    ok "nothing patches a disc's sector 0"
fi

exit $rc
