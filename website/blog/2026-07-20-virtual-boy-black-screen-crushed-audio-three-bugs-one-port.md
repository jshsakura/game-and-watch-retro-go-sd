---
slug: virtual-boy-black-screen-crushed-audio-three-bugs-one-port
title: 'Virtual Boy: black screen, crushed audio, and the DRC that couldn&rsquo;t run'
authors: [jshsakura]
tags: [fault, hardware]
image: /img/clock-hero.jpg
---

The Virtual Boy is Nintendo's red-and-black 3D console from 1995, a commercial
failure, an emulation curiosity, and — for one week — the most frustrating core
in this firmware. It compiled. It linked. The CPU ran. The screen stayed black.
The audio sounded like it was being fed through a distortion pedal. And the
release binary refused to link at all, for two completely unrelated reasons.

{/* truncate */}

This is the story of three bugs in the Virtual Boy port, each in a different
layer, each found by a different method, all in the same week.

## Bug one: the black screen that divided by zero

The VB core is red-viper, a 3DS port originally written to run inside a 3DS
homebrew GUI (`vb_gui.c`). On the 3DS, the GUI performs a sequence of
initialisation steps before handing control to the emulator — setting up
the render mode, computing the frame timing, picking the display path. On
our device, there is no GUI. We call `v810_reset()` and start the emulator.
Everything the GUI would have set up, we have to set up ourselves. We missed
two things.

The first was **`tVBOpt.RENDERMODE`**. red-viper's default is `RM_TOGPU` —
"render to the 3DS's GPU." That is the fast path on a 3DS, where the GPU is
sitting right there. We do not have a 3DS GPU. We use red-viper's software
VIP (the Virtual Boy's Video Image Processor, emulated in C, drawing to a
framebuffer in RAM). `RM_TOGPU` means "the renderer's output goes nowhere
we can read." The software VIP path is `RM_CPUONLY`. The default was wrong
for us; nobody had changed it.

The second was **`tVIPREG.frametime`**. This is the per-frame timing value
the VIP uses to decide how fast to scan out the framebuffer. On the 3DS,
the GUI calls `videoProcessingTime()` after measuring the actual hardware
frame rate, and writes the result into `tVIPREG.frametime`. Without that
call, `frametime` stays at its default of **zero**.

Zero is the worst possible value. The VIP's draw-row progress is computed
as `current_cycle / frametime`. With `frametime = 0`, that is a **divide
by zero**. On x86 (the host harness), integer divide-by-zero raises
`SIGFPE` and crashes — loud, obvious, found immediately. On the **Cortex-M7**,
integer divide-by-zero **yields zero silently**. No fault. No exception.
No crash. Just a progress value of zero, forever.

The draw-row never completes. The framebuffer-flip condition (`!drawing`)
never becomes true. The LCD displays `fb0` — the framebuffer that was
initialised to all-black at boot and never written to — for the entire
run. The game is running. The CPU is executing instructions. Brightness
is set. But the framebuffer never flips, because the draw never finishes,
because the timing is zero, because we forgot one line of GUI
initialisation that the original port assumed a human-readable GUI would
always do.

Fix, after `v810_reset()`:

```c
tVBOpt.RENDERMODE = RM_CPUONLY;
tVIPREG.frametime  = videoProcessingTime();
```

Two assignments. Wario Land reaches real gameplay frames. The black screen
disappears.

The lesson: **ARM does not trap integer divide-by-zero.** x86 does. Every
host harness that divides by zero crashes and is fixed; every device build
that divides by zero silently produces zero and the bug hides. The host
caught this one. The device would not have.

## Bug two: the crushed audio

With the screen working, the audio was wrong. Not absent — present, audible,
recognisable as the game's soundtrack — but **crushed**. Harsh. Buzzing
where there should have been tone. The word the user used was "crushed,"
and it was the right word, because what was happening was exactly the thing
"crushed" describes in audio engineering: **aliasing**.

The Virtual Boy's VSU (the sound chip) runs at one sample rate. The device's
audio output path runs at another (set per-app via `odroid_system_init`'s
sample-rate argument, reconfiguring PLL2). The ratio is about **2.3×** —
the VSU produces 2.3 samples for every one the device wants. We have to
decimate: throw away 1.3 samples per output sample, keeping one.

The question is *which* sample to keep. The cheapest decimator is **nearest
pick** — take every 2.3rd sample, drop the rest, no filtering. This is
fast and it is wrong. The samples you drop carry high-frequency content
(harmonics of the VSU's waveforms). When you drop them without filtering,
that high-frequency content **folds back** into the audio band as aliasing
— spurious low-frequency tones that were not in the original signal. A
pure square wave at 440 Hz, decimated by nearest-pick, comes back as 440 Hz
plus a swarm of buzzy non-harmonic tones. It sounds crushed. It sounds
exactly like the user's report.

Fix: **2-tap averaged resample.** Before dropping a sample, average it with
its neighbour. That is a one-pole, one-zero low-pass filter — the cheapest
filter that exists, the same box-car low-pass the PCE CD-DA decimator
already uses elsewhere in the firmware. It kills the harmonics above the
Nyquist of the target rate before they can fold back. The 440 Hz square
wave comes back as a 440 Hz square wave. The crushing is gone.

The lesson: **decimation without filtering is aliasing.** It does not matter
how fast the nearest-pick is. The mathematics of sampled signals will not
let you throw away samples and keep the spectrum. You filter first, then
drop. Every resampler in the firmware now does this; the VB was the last
one that didn't, and its audio was the one that sounded like it.

## Bug three: the DRC that couldn't run

The first two bugs were runtime. The third was link-time, and it was the
one that almost killed the port.

red-viper has two execution paths for the V810 CPU (the Virtual Boy's
processor):

- **The interpreter** — reads each instruction, decodes it, executes it.
  Slow, simple, portable. This is what we want on the device.
- **The DRC** (Dynamic Recompiler) — translates V810 blocks into native ARM
  code at runtime, caches them, executes the native code directly. Fast,
  complex, requires a writable code page and a CPU that can execute
  generated ARM instructions.

Whether the DRC is built is controlled by `DRC_AVAILABLE`. red-viper sets
it based on the platform: if the target is ARM and ARMv7, `DRC_AVAILABLE =
true`. The Cortex-M7 **is** ARMv7. So `DRC_AVAILABLE` came out `true`. The
build compiled `v810_cpu.c` in DRC mode. The DRC path emits A32 (ARM's
32-bit instruction set) code at runtime and expects to execute it.

But the Cortex-M7 is **Thumb-2 only**. It cannot execute A32 code. The A32
DRC path, even if it ran, would fault on the first generated block. And
worse: `drc_core.c` — the file that *implements* the DRC — was not in our
build list, because the documented intent was "interpreter only on device."
So `v810_cpu.c` emitted references to `drc_handleInterrupts`,
`drc_relocTable`, and a dozen other `drc_*` symbols that had no definition.
The link failed with `undefined reference to drc_*`.

Fix: force `DRC_AVAILABLE = false` on `GNW_VB_DEVICE`. The interpreter is
what the device uses; the DRC was never going to work on a Thumb-2-only
core. Plus a guard on the one unguarded assignment to a DRC-only struct
field, so the interpreter path doesn't try to write to a field that doesn't
exist.

And then the **second link error**: `multiple definition of sound_init` and
`sound_update`. red-viper's `vb_sound.c` defines `sound_init` and
`sound_update`. smsplusgx (the Master System / Game Gear core, a completely
different emulator) also defines `sound_init` and `sound_update`. Both are
overlays linked at the same address — so the firmware's linker binds them
together, quietly, and the VB's `sound_init` ends up calling the SMS's
`sound_update`, or vice versa, depending on which overlay's symbol the
linker saw first.

This is the **overlay aliasing** trap that CLAUDE.md warns about in detail
— the same trap that drove Super Metroid's bus through Super Mario World's
`g_snes` for three releases
([that story](/devlog/super-metroid-three-releases-that-couldnt-boot)). The fix
for the VB was the lightest possible: compile the VB device objects with
`-Dsound_init=vb_sound_init -Dsound_update=vb_sound_update`. A preprocessor
rename, flag-only, no source churn, host harness unaffected. The VB's sound
functions live in their own namespace; the SMS's live in theirs; the linker
can no longer confuse them.

## The through-line

Three bugs, one port, one week.

1. A **silent divide-by-zero** — ARM does not trap it, x86 does, and the
   device showed a black screen while the host would have crashed.
2. A **decimation without filtering** — the cheapest resampler is the
   wrongest one, and "crushed" is the exact word for what it sounds like.
3. A **DRC for the wrong CPU** — the build system saw ARMv7 and enabled a
   code path that requires A32, on a core that can only run Thumb-2, and
   the link failed in a way that looked like a missing file rather than a
   wrong configuration.

Each one was a different shape. Each one was found by a different method —
the first by the host harness crashing where the device didn't, the second
by listening to the user's word ("crushed" is a technical term and they
used it correctly), the third by reading the link errors and realising the
DRC was never supposed to be built. And each one, on its own, would have
been enough to kill the port. All three, in sequence, in one week, on a
console that sold poorly enough in 1995 that emulating it twenty years
later on a Nintendo keychain feels like a private joke.

The Virtual Boy runs now. The screen draws. The audio is clean. The link
is green. And the three lessons sit in CLAUDE.md — ARM doesn't trap what
x86 traps, decimation without filtering is aliasing, and a build flag that
enables a feature for a CPU family is not the same as that feature being
usable on every member of that family — waiting for the next port to
rediscover them.
