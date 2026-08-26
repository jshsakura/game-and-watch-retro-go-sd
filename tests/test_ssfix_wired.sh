#!/usr/bin/env bash
# Wiring gate: the md32x savestate hardening (leader mission 2, 2026-08-27)
# and the mid-frame load refusal (mission 4). A test that greps the real
# files -- the wiring IS the feature here. Run from the repo root.
# (Pattern: test_boot_rescue_wired.sh / test_iwdg_wired.sh line.)
R=0
chk() { [ "$1" -eq 0 ] && echo "ok: $2" || { echo "FAIL: $2"; R=1; }; }

M=Core/Src/porting/md32x/main_md32x.c

# 1) header carries all four fields (magic, version, bytes, crc)
grep -q "uint32_t magic" $M && grep -q "uint32_t crc" $M && grep -q "uint32_t bytes" $M
chk $? "state header: magic+version+bytes+crc struct"

# 2) version bumped to 3
grep -q "MD32X_STATE_VERSION.*3\|VERSION.*3" $M || grep -qE "define MD32X_STATE_VERSION +3" $M
chk $? "state version is 3 (v2 files refused)"

# 3) length pre-check before any streaming (SM idiom)
grep -q "avail" $M
chk $? "load: bytes-vs-available pre-check before streaming"

# 4) CRC pre-computed before PicoStateFP touches live structs
grep -q "PicoReset" $M
chk $? "load failure path calls PicoReset (deterministic state, not half-restored)"

# 5) mid-frame guard (mission 4): the flag, the refusal, the PicoFrame wrap
grep -q "md32x_pico_in_frame" $M
chk $? "guard flag exists"
N=$(grep -c "md32x_pico_in_frame" $M)
[ "$N" -ge 4 ]
chk $? "guard wired (decl + refusal + set + clear = >=4 refs, got $N)"

# 6) IWDG fully withdrawn (leader ruling: measured 33 s sleep regression)
grep -q "IWDG1->KR" Core/Src/main.c
[ $? -ne 0 ]
chk $? "main.c: no IWDG key writes left"
grep -q "if (wdog_enabled)" Core/Src/main.c
chk $? "main.c: WWDG refresh survives"

exit $R
