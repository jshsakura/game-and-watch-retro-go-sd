---
slug: the-caller-that-never-called
title: 'The caller that never called: four bugs in one core, one shape'
authors: [jshsakura]
tags: [homebrew, testing, fault]
image: /img/clock-hero.jpg
---

Four separate faults shipped in the same homebrew port. The pause menu did not
exist. Save and Load did nothing. Autosave lost your pet whenever you quit to the
launcher. The volume shortcut changed the volume with nothing on screen to say so.

Four bug reports, four different symptoms, four different subsystems — and every
single function involved was correct. Nothing called them.

{/* truncate */}

## The port

TamaPoke is a virtual-pet game written for an ESP32 board with a 466×466 round
touch panel. Porting it to the Game &amp; Watch means a 320×240 rectangle, no touch
panel, and a launcher that owns the hardware. The game logic came over unmodified;
what had to be built was the layer between it and the firmware.

That layer is where all four bugs lived, and they are worth putting side by side
because the shape repeats so exactly.

## 1. The pause menu

Every core in this firmware calls two functions per frame:

```c
common_emu_frame_loop();                      // paces the frame
common_emu_input_loop(&joystick, options, &repaint);   // the PAUSE menu
```

The port called the first one. Brightness, volume, speed, save, load, exit,
shortcuts — all of it lives in the second, and the second was never called.

The names do not help. Both are `common_emu_*_loop`, both are called once per
frame, both take the frame's state. But pacing a frame is not menuing a frame.
**Having one is not having the other**, and nothing in the type system, the linker
or the compiler has an opinion about that.

## 2. Save and Load

The launcher's pause menu offers Save and Load rows for every app. It populates
them from handlers the core registers:

```c
odroid_system_emu_init(&load_state, &save_state, ...);
```

That call was missing. The rows still appeared — the menu offers them either way —
so the game looked like it supported savestates and silently did nothing when you
used them. A row that lies is worse than a row that is absent.

This exact omission had shipped once before in this repo, in the Super Metroid
port. Same function, same result, different year.

## 3. Autosave

A virtual pet has no save file the player manages: it is a clock that keeps
running, so it has to persist by itself. It writes constantly, and the SD card
must not be touched mid-play — a write in the middle of the frame loop is how the
FAT gets corrupted. So the store lives in RAM and is flushed at safe points.

The safe points are the launcher's, and they are hooks the core has to *ask for* —
the fourth and sixth arguments of the same `odroid_system_emu_init()` above:

- `sram_save` fires on every sleep or standby entry, **and** inside
  `odroid_system_switch_app()` before the card is unmounted
- `shutdown` fires on power-off

Both were `NULL`. So the pause menu's Save row worked, and the idle timeout worked,
and **quitting to the launcher, holding POWER, and the low-battery auto-off each
discarded everything since the last commit**. Meanwhile `Pet::save()` and
`prefs_commit()` were perfect, well tested, and never reached.

## 4. The shortcut overlay

Reported as "the volume and brightness shortcuts do not work". They worked. Hold
PAUSE and press left: the volume really did go down. There was simply nothing on
screen to say so, which from the sofa is indistinguishable from a dead shortcut.

`common_emu_input_loop()` *acts* on the shortcut and then **arms** an overlay.
Drawing it is a separate call, `common_ingame_overlay()`, that each core makes
between its own render and the buffer swap. Twenty-nine cores in this tree call
it. This one did not.

## The thing they have in common

None of these is a logic error. There is no wrong branch, no off-by-one, no
misunderstood spec. In every case the callee was fine and the call site was empty,
and that is a category of bug with two nasty properties:

**A unit test of the function proves nothing.** You can test `common_ingame_overlay()`
until it is bulletproof. It will still never be called.

**The symptom points somewhere else.** "Save doesn't work" sends you into the
savestate code. "The overlay is broken" sends you into the overlay drawing. Both
are innocent, and you can read them for an hour without seeing anything wrong —
because there is nothing wrong.

## What actually catches it

A test that asserts the *contract at the call site*. Not "does this function work"
but "does anyone call it".

```bash
# every core that takes the PAUSE shortcuts must also draw their feedback
for f in $(grep -rlE '^[^*/]*common_emu_input_loop\(' Core/Src/porting); do
    grep -qE '^[^*/]*common_ingame_overlay\(' "$f" || fail "$f"
done
```

Six lines. It would have caught the fourth bug the day it was written, and it
catches the next core that forgets. There are four of these in the tree now
(`test_ingame_overlay_wired.sh`, `test_tamapoke_save_wired.sh`,
`test_idle_timeout_wired.sh`, `test_boot_rescue_wired.sh`), and every one of them
exists because something shipped without its caller.

Two details, learned the hard way while writing that one:

**Match a call, not a name.** The first version grepped for the word
`common_emu_input_loop` and accused a file whose only mention of it is a *comment*
explaining what the real caller does. `^[^*/]*name(` skips comment bodies.

**Do not ask `make` what is built.** The second version asked `make -pn` which
files the build compiles, so it could skip dead code. Three consecutive runs
checked 27, 26 and 27 cores, with different files sliding into "not built" each
time — a dry run re-evaluates the Makefile's `$(shell …)` calls. **A gate whose
scope silently varies is worse than no gate**: a green run proves nothing about the
file it quietly dropped. It now checks everything and names the single dead file
with its reason, plus a floor — fewer than twenty cores matched means the scan is
broken, not the tree.

## The uncomfortable part

The port had a host harness the whole time. It compiled `tamapoke_input.cpp` from
the Makefile's own source list, rendered every screen, and reported green.

It never *called* the input layer.

So the harness was compiling the file, linking the file, and proving nothing about
it — which is its own version of the same bug, one level up. Rendering a screen is
not using it. The harness now presses every button on every screen, including with
a stale focus index, and the first time it did, it found a null-pointer
dereference that had been crashing the device on any keypress.
