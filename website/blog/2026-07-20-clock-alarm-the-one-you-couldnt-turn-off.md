---
slug: clock-alarm-the-one-you-couldnt-turn-off
title: 'The clock alarm you couldn&rsquo;t turn off'
authors: [jshsakura]
tags: [clock, fault, hardware]
image: /img/clock-hero.jpg
---

The Clock app is the thing this device *is* when you buy it. Nintendo shipped it
as a clock that plays games; we shipped it back as a games console that tells
the time. So of course the alarm had bugs. Four of them. Each one, on its own,
made a rung alarm feel inescapable. Together they made the user pull the
battery.

{/* truncate */}

This is the story of the alarm that wouldn't stop ringing — four separate
device-only bugs, each reported as "the alarm went off and nothing would stop
it," each with a different root cause, all in the same feature, found over
about a week of device reports I could not reproduce on the host.

## Bug one: the confirm button was snooze

The user dismisses a ringing alarm by pressing **A**. A is the confirm button.
Everywhere else in the launcher, A means "yes, I want this." So the user
presses A, the alarm stops, they think they're done.

`A = DISMISS` was wired as `A = SNOOZE`. The alarm stopped, but only for
`SNOOZE_MS` — five minutes. Then it rang again. The user had walked away by
then. They came back to a ringing device and pressed A again. Snooze. Five
minutes. Ring. Press. Snooze. Ring.

And there was a worse version: if the user, annoyed, went into the alarm
editor to switch the alarm *off*, the pending snooze was not cleared. The
editor changed the alarm's enabled flag, saved, exited — and five minutes
later the snooze that the editor knew nothing about fired anyway. The user
had told the device "this alarm is off." The device rang it regardless.

Fix: leaving the alarm editor cancels any pending snooze. The user has just
told us what they want their alarms to be; we honour that.

## Bug two: setting the clock fired the alarm

Reported as: *"I changed the time and the alarm suddenly went off."*

`alarm_should_fire()` asks one question: *does the current minute equal an
enabled alarm's minute?* It cannot tell whether that minute **arrived**
(watch it tick over) or whether the user **jumped onto it** (set the clock
forward to it). `clock_edit_time()` writes the RTC and returns; the very next
pass of the main loop sees `hh:mm == the alarm` and rings.

And the "+ Add alarm" default is **07:00, enabled**. So if the user opens the
alarm editor, adds an alarm they intend to set for later, leaves it at the
default, goes back to the clock, and sets the time to 7:00 to see what the
dial looks like — the alarm rings. Instantly. Every time.

The editor had the same defect from the other direction: on exit it set
`s_last_fired_min = -1` ("re-arm"), so dismissing a ringing 07:00 alarm and
then *opening the alarm list* — still at 07:00 — rang it straight back.

Fix: `alarm_claim_minute()`. The minute you are standing on counts as
already-fired. `alarm_should_fire()` clears the guard as soon as the minute
changes, so a later alarm still rings today, and 07:00 rings normally
tomorrow. The host test (`tests/test_clock_alarm.c`) pins both directions
plus "an alarm set for 07:05 still rings at 07:05."

## Bug three: the preview froze the device

The alarm settings menu had a **tone preview** — press an option, hear the
alarm sound for a moment, pick the one you like. Nice idea. It froze the
device.

`alarm_tone_audition()` ran for `ALARM_RING_MS` — **sixty seconds**. With
the settings dialog frozen behind it. The user opened the alarm settings,
picked a tone, and the device locked up for a full minute with the tone
playing. No input. No escape. The only way out was the battery.

And it only escaped on a key **edge** — a press that was already down when
the preview started (the same press that *triggered* the preview) was never
seen as a new press. The user held the button, nothing happened, they let
go, nothing happened, the preview ran its full sixty seconds. The SD file
preview next to it, by contrast, capped at ten seconds. Two previews, two
caps, one of them sixty times longer than the other.

The fix was the honest one: **remove the preview entirely.** The alarm still
rings normally at its set time via the main-loop ring path. The preview was
the *only* code that fed an audio tone from inside the settings menu; every
other tone comes from the real ring. Taking the suspect out removed the
freeze. It also freed 776 bytes of internal flash, which we needed (see bug
four).

The lesson here was the same one the Super Metroid harness story teaches:
**the bug is in the thing that was just added.** The owner's report was
"the freeze appeared with the alarm preview and never happened before it."
That sentence *is* the diagnosis. I had been looking for the bug in the ring
logic for a day before I read the report again and removed the preview.

## Bug four: the RTC alarm that stayed armed while awake

This one was the worst, because it was nowhere near the alarm code.

The Clock app, when it goes to sleep, arms **RTC Alarm A** with its interrupt
enabled (`HAL_RTC_SetAlarm_IT`, `NVIC RTC_Alarm_IRQn` on). The STM32's RTC
can wake the chip from sleep — that is how a sleeping clock rings at its
alarm time without burning battery. Reasonable.

Nothing disarmed it on wake.

Boot cleared the ALRAF flag (the "alarm fired" bit) but left **Alarm A
armed** — matching the same daily H:M:S, interrupt still enabled. So while
the device ran **awake**, the RTC alarm interrupt stayed live. When the
clock next reached the alarm's minute, the RTC fired the interrupt on the
*running firmware* — and the running firmware was not expecting it. The IRQ
handler, wired only for the sleep-wake path, did something the awake state
machine never anticipated. The device froze: frame stuck, SAI DMA still
replaying the last audio buffer = a continuous tone, no input. From the
user's side: "I opened the alarm settings and it rang and nothing but a
battery pull stopped it."

And it got worse after the auto-sleep change (which slept, and thus armed
the alarm, far more often). The more the device slept, the more often the
alarm was armed when it woke, the more likely the freeze.

Fix: `rg_alarm_disarm_rtc()` — deactivate Alarm A and clear the flag, called
on every boot *after* the wake cause is latched (so it cannot affect the
wake decision). The next sleep re-arms it. The awake alarm rings via the
epoch cache poll, which never needed the interrupt.

**I could not reproduce this on the host.** The host has no RTC. The host's
alarm logic is pure software and works correctly. The bug was in the
*contract* between the RTC peripheral and the firmware's sleep/wake state
machine — a contract that only exists on the device.

## The through-line

Four bugs. One feature. Each one reported as "the alarm went off and nothing
would stop it." Each one a different root cause:

1. **A semantic confusion** — confirm and snooze on the same button.
2. **A missing distinction** — "the minute arrived" vs "I jumped onto the
   minute."
3. **An unbounded preview** — a tone that played for sixty seconds with no
   escape, in code that was only ever supposed to play for ten.
4. **A peripheral contract** — an RTC alarm armed for sleep, never disarmed
   on wake, firing its interrupt on a running firmware that did not expect
   it.

The host caught none of them. The host's alarm logic was, and remains,
correct. The bugs were all in the wiring between the logic and the device —
the button mapping, the RTC write, the audio path, the interrupt mask. The
logic was never wrong. The thing that never got wired correctly was
everything around it.

And the user pulled the battery, every time, because there was no other way
out. That is the thing I keep from this story: **a device with no reset
button and no escape hatch is a device where every freeze is a battery
pull.** The [boot rescue work](/devlog/boot-rescue-when-a-hung-boot-was-a-dead-battery)
was already underway by then — the watchdog + DR28 counter that stops a
hung boot at a rescue screen — but the alarm freezes were a reminder of why
that work mattered. A device you cannot reset is a device you cannot trust
with anything that might hang.

The alarm works now. It rings when it should, stops when you dismiss it,
stays quiet when you set the clock onto its minute, and disarms its RTC
interrupt when the device wakes. And the preview is gone, because some
features are not worth the freeze they bring with them.
