---
slug: boot-rescue-when-a-hung-boot-was-a-dead-battery
title: "When a hung boot meant a dead battery — adding a rescue screen to a sealed device"
authors: [jshsakura]
tags: [boot, fault, hardware]
image: /img/clock-hero.jpg
---

The Game & Watch has no reset button. The shell is glued shut. The power button
is a GPIO the firmware polls, not a hard power cut — which means that for a long
time, the only way out of a boot that hung was to **let the battery go flat**.
On a device that lives in a pocket, that is a genuinely bad afternoon. This
entry is the short version of how a boot rescue screen came to exist, and the
small, honest counter that makes it work.

{/* truncate */}

## What the fault looked like before

A bad build would hang somewhere in `main()` — SD init, config load, the
auto-resume step, anywhere — and the screen would sit lit until the battery
died. There was a BSOD already, but it only fired if the code reached the fault
handler: a hang that simply never returned never reached it. And the BSOD itself
was almost uselessly terse — **the title says "Hardfault" and gives you a PC and
an LR and nothing else.** No `CFSR`, no `HFSR`, no `BFAR`. BusFault, UsageFault
and MemFault are all disabled (`SCB->SHCSR` is never written), so every one of
them escalates to a plain "Hardfault". The title alone cannot tell an alignment
fault from a null deref from a wild store.

So a hang ended at a flat battery, and a crash ended at a title that named
nothing. Both were bad on a device you cannot open.

## The watchdog that was always there but never armed

The STM32 independent watchdog (`IWDG`) is a peripheral that resets the chip if
it is not periodically fed. It was available the whole time. The fix was simply
to **arm it at the top of `main()` on every boot**, before anything that could
hang. Once it is armed, a hang stops being an infinite lit screen — within a
bounded time the watchdog fires and the chip resets.

But a watchdog reset on its own is just a faster way to hang again. If the boot
that failed is the boot that will run after the reset, you have built a reboot
loop, not a rescue. The actual rescue needed **memory that survived the
reset**.

## The counter that survived

The STM32 has twenty backup registers (`TAMP.BKP0R`–`TAMP.BKP31R`, what the
HAL calls `RTC->BKP0R`…`BKP31R`) — tiny four-byte slots in the always-on domain
that survive about everything except removing power. Most of them were already
spoken for: `DR0` is the original-firmware boot flag, `DR1` is the alarm epoch,
`DR29` holds a clock snapshot, `DR30` the charger state. **`DR28` was free**, so
it became the consecutive-failed-boot counter.

The rule is small: every cold boot increments `DR28`, and **"boot succeeded"**
clears it back to zero. The trick was defining "boot succeeded" honestly. It
cannot be "reached the end of `main()`", because `main()` does not return. It
cannot be "the launcher drew a frame", because the launcher is one of the
things that can hang. The definition that held is: **the shared input poll
(`odroid_input_read_gamepad`) has run at least 300 times *and* the device has
been alive for at least 8 seconds.** Anything that has answered the buttons 300
times for 8 seconds is not hung. A deliberate sleep also clears the streak, so
going to sleep does not count against you.

When `DR28` reaches 3, the boot stops *before* SD init, config load, and
auto-resume — the three places a bad build is most likely to die — and offers a
rescue screen instead: launcher-only boot, normal boot, or power off. After 60
seconds of inactivity it powers itself off, so even a rescue screen you walk
away from does not flatten the battery. And **POWER on the BSOD really powers
off** — the same GPIO read that does nothing useful in a hang now does the
obvious thing on the screen you actually see it from.

## What it took to trust

The wiring spans five files. The counter is read in one place, incremented in
another, cleared in a third, gated on in a fourth, and offered to the user in a
fifth. That is exactly the shape of thing that works once and then silently
rots when someone refactors one of the five and the others stop agreeing. So
the contract is pinned by two tests:

- **`tests/test_boot_rescue_wired.sh`** asserts that every loop that can idle
  asks the one rule, that nobody re-derives it, and that the counter is touched
  in exactly the places it should be. It is the shape of test that catches a
  caller that never calls — the same lesson Super Metroid taught.
- **`tests/test_boot_rescue.c`** runs the real counter against a fake backup
  register, so the increment/clear/threshold logic is exercised without a
  device.

## What I actually learned

The fault handler you do not wire is the fault handler you do not have. The
watchdog was there the whole time. The backup registers were there the whole
time. The power-button GPIO was there the whole time. None of them helped,
because nothing *asked* them to help — and "asked" here means a five-file
contract that has to be wired in five places and then *pinned by a test that
fails when any of the five goes silent*.

A hung boot is not a hardware problem. It is a software problem that did not
have a place to land.
