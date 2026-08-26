#!/bin/bash
# The bug was not in the ADC reading. It was in where the reading ran.
#
# bq24072_poll() ran in TIM1_UP context -- priority (0,0), above SysTick --
# and HAL_ADC_Start_IT/Stop_IT contain enable/disable wait loops that time
# out on HAL_GetTick(). With ticks frozen, "wait for ADRDY" is not slow, it
# is infinite: the one time the ADC did not come ready, the ISR spun forever
# and starved main (reproduced 2026-08-26 during the watchdog hunt). Every
# 1 Hz poll entered that loop, because the window-end Stop_IT disabled the
# ADC each time.
#
# Pin the wiring so the shape cannot come back:
#   - TIM1_UP may only request (flag), never poll;
#   - the burst is serviced from odroid_input_read_gamepad, the one
#     main-context function every app loop already calls;
#   - bq24072.c may never call HAL_ADC_Stop_IT again -- with the ADC left
#     enabled, no context can reach the enable/disable wait loops at all.
set -u
rc=0

fail() { echo "  FAIL $1"; rc=1; }
ok()   { echo "  OK   $1"; }

# 1. TIM1_UP requests; the bare poll must be gone from the handler block.
if grep -q "bq24072_poll_request();" Core/Src/stm32h7xx_it.c; then
    ok "TIM1_UP requests the battery poll instead of running it"
else
    fail "TIM1_UP never calls bq24072_poll_request() -- the 1 Hz kick is gone or blocking"
fi
if sed -n '/TIM1_UP_IRQHandler/,/^}/p' Core/Src/stm32h7xx_it.c | grep -q "bq24072_poll();"; then
    fail "TIM1_UP still calls bq24072_poll() directly (ISR-context HAL enable path)"
else
    ok "TIM1_UP block contains no direct bq24072_poll()"
fi

# 2. The cooperative consumer exists and is wired into the universal input read.
grep -q "void bq24072_poll_service(void)" Core/Src/bq24072.c \
    && ok "bq24072_poll_service() defined" \
    || fail "bq24072_poll_service() missing"
grep -q "bq24072_poll_service();" Core/Src/porting/odroid_input.c \
    && ok "odroid_input_read_gamepad() services the poll (every app loop)" \
    || fail "no main-context service point -- flag set at 1 Hz, never consumed"

# 3. The disable-wait is unreachable from any interrupt: the ONE allowed
#    Stop_IT call lives inside bq24072_adc_sleep (main context, before
#    STOP2). Anywhere else in the driver is a regression.
N=$(grep -cE "HAL_ADC_Stop_IT[[:space:]]*\(" Core/Src/bq24072.c)
IN_SLEEP=$(sed -n "/void bq24072_adc_sleep/,/^}/p" Core/Src/bq24072.c | grep -cE "HAL_ADC_Stop_IT[[:space:]]*\(")
if [ "$N" -eq "$IN_SLEEP" ]; then
    ok "HAL_ADC_Stop_IT exists only inside bq24072_adc_sleep (main context)"
else
    fail "HAL_ADC_Stop_IT called outside bq24072_adc_sleep ($N total, $IN_SLEEP in sleep)"
fi

# 4. Declarations present (a caller with no prototype would compile as int).
grep -q "bq24072_poll_request" Core/Inc/bq24072.h \
    && ok "request/service declared in bq24072.h" \
    || fail "bq24072.h is missing the request/service declarations"

# The ADC may stay enabled while awake, but STOP2 must quiesce it -- the
# µA budget of a device people sleep for weeks depends on it.
if grep -q "bq24072_adc_sleep" Core/Src/gw_sleep.c; then
    echo "  OK   STOP2 entry quiesces the ADC (bq24072_adc_sleep in gw_sleep.c)"
else
    echo "  FAIL gw_sleep.c never calls bq24072_adc_sleep -- STOP2 leaks ADC current"
    rc=1
fi

exit $rc
