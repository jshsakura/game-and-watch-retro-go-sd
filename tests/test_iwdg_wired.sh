#!/bin/bash
# Wiring gate for the IWDG last resort (leader m2937 #3).
# A safety net that silently fails to arm is worse than none -- so the checks
# pin: module enabled, started next to the WWDG, refreshed OUTSIDE the WWDG
# gate (once started it cannot be stopped), and the armed proof is a PR
# readback into DR15 bit3, not HAL_OK. Run from the repo root (run.sh does).
rc=0
chk() { if [ "$1" = "0" ]; then echo "OK   $2"; else echo "FAIL $2"; rc=1; fi; }

grep -q "IWDG1->KR = 0xCCCCu" Core/Src/main.c
chk $? "main.c: IWDG start key written in wdog_enable (parallel to WWDG)"

grep -q "IWDG1->PR = 6u" Core/Src/main.c
chk $? "main.c: prescaler 256 via PR=6 (~32 s window)"

REF_POS=$(grep -A10 "void wdog_refresh" Core/Src/main.c | grep -n "IWDG1->KR = 0xAAAAu" | head -1 | cut -d: -f1)
GATE_POS=$(grep -A10 "void wdog_refresh" Core/Src/main.c | grep -n "if (wdog_enabled)" | head -1 | cut -d: -f1)
[ -n "$REF_POS" ]
chk $? "main.c: IWDG refreshed in wdog_refresh"
if [ -n "$REF_POS" ] && [ -n "$GATE_POS" ] && [ "$REF_POS" -lt "$GATE_POS" ]; then
  echo "OK   main.c: IWDG refresh is OUTSIDE the wdog_enabled gate (unstoppable dog must be fed)"
else
  echo "FAIL main.c: IWDG refresh placement"; rc=1
fi

grep -q "IWDG1->PR != 0u" Core/Src/gw_crash_crumbs.c
chk $? "crumbs: DR15 bit3 armed-proof is a PR readback, not HAL_OK"

grep -q "bit3 IWDG" Core/Inc/gw_crash_crumbs.h
chk $? "header: DR15 map documents bit3"

exit $rc
