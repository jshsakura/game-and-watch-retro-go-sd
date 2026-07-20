---
slug: the-fader-the-game-forgot-to-cancel
title: 'PCE CD: the fader the game forgot to cancel'
authors: [jshsakura]
tags: [pce, audio, hardware]
image: /img/clock-hero.jpg
---

A game's music is silent. The rip is good — you can hear it on a different
emulator. The CD-ROM bios boots, the ADPCM engine is alive, the PCM RAM is
full of valid samples. Everything is wired. The music is still silent.

This is the PCE CD audio story. Nine times out of ten the silence is not a
bug in your emulator. It is a fader the game set and forgot — and the
peripheral remembered.

{/* truncate */}

## The fader that has no "fade in"

The PC Engine CD has a CD-DA / ADPCM volume fader at register `$180F`. It is
asymmetric in a way that surprises people:

- Write `$180F` with bit 3 set (`$08`–`$0F`): start a **fade out**. Target
  depends on the low nibble — CD-DA, ADPCM, both — over 2.5 s or 6 s.
- Write `$180F` without bit 3 (`$00`–`$07`): **cancel any active fade** and
  restore full volume (65536).

There is no "fade in" command. If a game wants the music to swell, it has to
cancel the fade and trust the user not to notice the step. Most games do
this promptly. *Dracula X* does not always.

The cinematic before the title fades the CD-DA out — a clean
`$180F = $0C`. Then the title screen starts. The title's BGM is the same
CD-DA track, picked up mid-stream. The game expects the fader to be at
resting full volume. It is at **zero**. The fade-out was never cancelled.
The track is playing. The samples are correct. The volume register is
silent. You hear nothing, and you blame your rip.

## The diagnostic that names it

`pce_scsi.c` logs every `$180F` touch to `pcecd_diag.txt`. The read is:

```bash
cd linux && rm -f pcecd_diag.txt
./build/retro-go-pce --syscard syscard3.pce "cd_pce/Dracula X.cue"
grep FADER pcecd_diag.txt
```

You are looking for three lines:

- `FADE_START target=CDDA` — the cinematic started a fade-out.
- `FADE_CONT` lines while the fade runs.
- A later `CANCEL` — the game restored volume before the next BGM.

If `CANCEL` never comes, or comes after the BGM has already started, you
have the bug. The companion lines:

- `CDDA_VOL_0` while `play=1` — confirmed stuck-at-zero.
- `WARN_LOW_CDDAVOL` — fading but not yet zero, still inaudible.

Compare with *Ys I & II*, which never touches `$180F` (or cancels within a
frame of starting a track). If Ys is audible and Dracula X is not, the rip
is good and the fader is the bug.

The fix is not in your emulator — the fader is correct, the game is wrong,
you are faithfully emulating a forgotten fade. But you can ship a host-side
patch in the launcher that cancels fades on track change, if you decide the
user experience matters more than cycle-exact silence. We chose to document
it and let the user cancel manually; the diag is the support tool.

## The ADPCM dropouts that were not the fader

A different silence: voices cut out after 4–5 seconds of playback. ADPCM
audio is 64 KB of double-buffered RAM, filled by DMA from the CD, drained
by the audio engine at the programmed rate. When the drain stops, it is
almost never the fader. It is the **DMA handshake**.

The ADPCM engine is Mednafen-style. `$180A` reads and writes go through
`ReadPending` / `WritePending` (57 / 33 cycles). `EndReached` fires when
`LengthCount` hits zero during a fetch or a read-complete — **not** when
the game manually stops at `$180D`. The half/end flags (`HALF`, `END`) and
`effective_port3()` are what gate the next DMA block. If you call the sync
at the wrong time, the DMA never completes, the second buffer never fills,
and after one buffer's worth of audio (~4–5 s) the engine runs dry.

The rule that took the longest to find:

> Call `pce_adpcm_sync()` only on status/DMA polls (`$1800`, `$1803`,
> `$180A`, `$180B`, `$180C`) plus once per frame before `Cycles` reset —
> **not** on every `$180x` read.

Calling it on every register read corrupts the IPL boot path (`$1808`).
The System Card's load routine polls `$1808` in a tight loop early in
boot, and an extra sync there advances ADPCM state the card does not
expect. The game prints `load error` and dies. Sync only on the registers
that actually advance time.

## Three more PCE footguns

- **`$180D` read must return the last command byte** (`s_last_cmd`), not
  zero. Some games poll `$180D` to confirm a SCSI command was accepted;
  returning `0` reads as "no command" and they re-issue forever, freezing
  the load.
- **Arcade Card physical reads must use the full 32-bit address**
  `(MMR[page] << 13) | (addr & 0x1FFF)`. The 13-bit window is just the
  *low* bits of the physical address; the page register supplies the high
  bits. Passing only the low 13 bits to the backing store corrupts
  streamed video — *Sapphire*'s intro is the canary, because it reads
  Arcade Card RAM in a tight pattern that hits every page.
- **CD-DA and SCSI data need separate file handles** in `pce_cd.c`. A
  `.cue` with the audio in a separate `.bin` from the data track will
  thrash the read head back and forth if you share one `FIL`. Two handles,
  two `f_seek` positions, no thrash.

## The lesson I actually learned

**Silence after loud audio is fader state, not a bad rip.** The peripheral
remembered what the game forgot. The CD-DA fader is a one-way knob with no
automatic restore — once a game writes a fade-out, the volume stays at the
destination until something writes a cancel. An emulator that faithfully
models the fader will faithfully reproduce the silence, and the bug report
will say "audio is broken in Dracula X." It is not broken. It is correct.
The game is wrong, and the hardware lets it be wrong.

The general shape: a peripheral with *state the game can set but cannot
implicitly clear* is a peripheral that will ship silence, or stuck inputs,
or wrong palettes, depending on what the game forgot. The fader is one.
The
[clock alarm that could not be turned off](/devlog/clock-alarm-the-one-you-couldnt-turn-off)
is the same shape on a different peripheral — state set on one path, not
cleared on the path that exits it. The diagnostic in both cases is the
same: log every touch of the register, read the log, find the missing
cancel.
