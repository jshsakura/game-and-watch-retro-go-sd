# 32X final chapter: verification plan (opened 2026-10-02)

Scope is verification only. No optimisation work is queued. Release under test:
`testbed-full-20261002-0140` (tag source `c07ace94` build, CI fix `6db21a61`).

## What is proven and what is not

| Claim | State |
| --- | --- |
| Doom 30.423 drawn fps, After Burner 22.77 | Measured in the optimisation arms, fixed boot and input window. Attract/scripted window, not play |
| Same numbers with the shipped package installed | **Unproven.** The measurement scripts swap only `32x.bin` and `32x.xip`; on 2026-10-02 the console showed about 12 fps while these notes said 22.8, because the optimised firmware had never been installed |
| Sound correct | Rig full-stream comparison only. Device `audio_sha256` is all zeros after `audio_stop_playing()` and is not evidence |
| Save, resume, CRC startup cost of a large cached ROM | Unchecked (#49) |
| Library-wide behaviour | Two titles measured. Chaotix and Kolibri were the slowest in the old ledger and have no number on this build |

## Install state (2026-10-02)

`tools/32x_devtools/install_release.py final-install-r1` installed bank2 plus 53
files (cores 33, lang 11, homebrew 9) from the final worktree. It backed up 55 files,
read back 54 identical, and started the firmware. Backup is under
`build/32x-mixer-hle-20261001/final-install-r1/backup`; `restore_release.py` undoes it.

After the start the target stopped answering SWD (`init mode failed`). The panel was
never photographed, so "launcher came up" is **not** established. Idle power-off was
not disabled. Likely STANDBY; a physical power button press is the only wake.

## Order of work once the console is reachable

1. Disable idle power-off first (`keepalive.sh on`, using the final worktree ELF, and
   read the value back before trusting it).
2. Photograph the panel (`tools/gnw_probe/screenshot.sh`). Launcher, not the
   corrupted-installation screen.
3. Read the build stamp off the device and compare it with the tag above. A trailing
   `+` fails the arm.
4. Matrix, one run per title, all on the installed package:
   Doom, After Burner Complete, Knuckles' Chaotix, Kolibri, Virtua Fighter, D32XR.
   Each number is bound to a screenshot taken in the same run, and the letters on
   the screen must name the title. A row whose screenshot shows another title is
   withdrawn.
5. Two windows per title, reported separately: attract (comparable with history) and
   injected input (play). Never swap them.
6. Sound, save and resume through the normal UI, then close #49.
7. Restore the user's idle timeout and commit it from a game loop.

## Rules carried over

An arm proves its identity before it counts (read back what was written, screen,
build stamp). Precision is not identity. A halted CPU changes what it measures, so
prefer no-halt reads.

## First pass on the installed release (2026-10-03)

Device identity: bank2 tag read off the device is `Retro-Go SD testbed-full-20261002-0140`
(no trailing `+`), launcher photographed, `/CONFIG` read back byte-identical to the
pre-install backup (idle timeout 600 s restored and committed).

Method: cold start per title through `start_rom.sh`, no input, `g_common_drawn_frames`
read twice 20 s apart from the shell. Shell timing includes SWD latency, so every figure
is approximate. These are **not** comparable with the 30.423 / 22.77 device windows
(different warmup and window) and are not play numbers.

| Title | drawn fps (approx) | What the panel showed 50 s in | Identity |
| --- | ---: | --- | --- |
| Doom | 37.2 | Doom title menu, logo and START/OPTIONS readable | confirmed |
| After Burner Complete | 24.4 | SEGA 1987-1994 title, "PRESS START BUTTON?" | probable, logo garbled |
| Chaotix | 22.6 | blue/cyan city scene, no text | **not confirmed** |
| Kolibri | 14.0 | NOVOTRADE INTERNATIONAL logo | publisher logo, so a logo screen, not gameplay |
| Virtua Fighter | 14.4 | AM2 logo | logo screen, not gameplay |
| D32XR | 11.3 | fire effect on black | **not confirmed**; the card may hold the 5 MiB release, not the 4 MiB build that read 41.5 |

Kolibri, Virtua Fighter and D32XR rows measure intro screens. The Chaotix and D32XR rows
are withdrawn until a screenshot names the title. Doom is the only clean row. Next:
longer warmup so each title reaches its own scene, and md5 of the ROM on the card.

## Second pass: 120 s warmup, then a 20 s window (2026-10-03)

Same method as the first pass, with a 120 s warmup so each title leaves its logos.
Figures are approximate (shell timing), no input, attract/demo scenes, not play.

| Title | drawn fps (approx) | Panel at the end of the window | Identity |
| --- | ---: | --- | --- |
| Doom | 27.0 | Doom start menu with the demo behind it, HUD readable | confirmed |
| After Burner Complete | 19.5 | in-flight scene, "LOCK ON", (C)SEGA 1987,1994 | confirmed |
| Chaotix | 18.0 | "Techno Tower" stage card, "CHAOTIX", LEVEL 2 | confirmed |
| Kolibri | n/a | **BSOD, `Usagefault`** | crashed inside the window; the row is void |
| Virtua Fighter | 10.8 | title screen, "PRESS START BUTTON" | confirmed (title screen, not a match) |
| D32XR | 6.2 | DOOM title / id Software copyright | the card holds the **5 MiB** build (md5 `4b1f584c...`), not the 4 MiB bench build (`110d2229...`) that read 41.5; not comparable |

Doom read 37.2 at 50 s and 27.0 at 140 s, so a single window is not a property of the title.

### Defect found on the installed release: Kolibri

BSOD: `Usagefault`, `PC=0x24051900`, `CFSR=0x01000000` (UFSR bit 24, UNALIGNED), about
2 minutes after a cold start. Within the md32x overlay the only function covering that
address is `gnw_line_pp` (`build/md32x/pico__32x__draw.o`, offset `+0x3d0`), and the
instruction there is `ldrd r4, r5, [r1]`: a 64-bit load, which Cortex-M7 traps unless
word aligned. Same class as the Super Metroid `STRD` fault in `CLAUDE.md`. The value of
`r1` at the fault was not captured, so which scene misaligns the source is unproven.
No fix is made here; it needs its own change, a RED reproduction, and a rebuild.

Device and card: `/CONFIG` read back byte-identical to the pre-install backup after both
sessions; idle timeout 600 s restored and committed.

## A/B: previous firmware against this release (2026-10-03)

A is the firmware that was on the device before the install: tag
`testbed-full-20260906-1924-61-g284aed79`. Its ELF was rebuilt from that commit with the
canonical flags and its `gw_retro_go_intflash.bin` is byte-identical to the bank2 backup
(261,864 bytes compared, backup tail is 0xFF padding), so symbols come from the right
program. A was installed by `restore_release.py` (bank2 plus 53 files, read back). B is
this release. Same scenario for both: cold start, no input, 120 s warmup, 20 s window.

| Title | A previous (approx fps) | B release (approx fps) |
| --- | ---: | ---: |
| Doom | 26.0 | 27.0 |
| After Burner Complete | 19.35 | 19.45 |
| Chaotix | 17.95 (359 frames) | 17.95 (359 frames) |

Screens match between A and B (Chaotix "Techno Tower" stage card, After Burner in flight),
so both ran the same scene. **No improvement is measurable in these windows.** The
likeliest reading is that these scenes are not CPU-bound: the previous firmware already
reaches the game's own frame pacing, so a faster core has nothing to show. That is a
reading, not a measurement. The 22.77 and 30.423 device windows of the optimisation series
were taken in different, heavier windows and are not reproduced here. Each A and B value is
one window, so no spread is known. The device is back on B, `/CONFIG` identical to the
original.

## Kolibri crash: fixed and checked on the device (2026-10-03)

Fix: `external/picodrive/pico/32x/draw.c`, the eight-pixel path now requires a word-aligned
`p32x` (was: even). The two adjacent u32 loads there fuse into one `ldrd`, which M7 traps
unless 4-byte aligned. A `p32x` that is 2 mod 4 falls to the quad path, whose single `ldr`
is legal unaligned. It was the only 64-bit load from `p32x` in `gnw_line_pp`; every `strd`
to `pd` was already behind an alignment test.

Gate: `tools/m7_qemu_rig/run_pp_align.sh` runs the real `draw.c` on QEMU Cortex-M7 with the
builder's compiler. RED on the pre-fix source: `UsageFault with UFSR.UNALIGNED`, fault
address `src+2`, the run never reaches PASS. GREEN with the fix: four alignments correct.
It is in `tests/run.sh` and skips, saying so, without qemu or a compiler.

Device: release `testbed-full-20260906-1924-76-g80ad6acb` installed (53 files read back),
tag read off the device and matched. Kolibri ran 200 s of warmup plus a 20 s window with no
BSOD, the panel showing the game (bird and enemies), drawn counter 5598 to 5858, and the
quit path worked. The same ROM crashed inside that window before. One run; the crash
needed a particular scene, so this is evidence and not a proof of absence. `/CONFIG` read
back identical to the original; idle timeout 600 s.

Where it lives: branch `fix/32x-octa-ldrd-align` in the final-release worktree (submodule
`64855fde`, main `80ad6acb`). Local only, not pushed, not merged into `testbed`.

## Speed work that landed after all (2026-10-04, PR #54)

The "SH-2 is closed" verdicts for Kolibri's slave, Chaotix and Virtua Fighter were read off
a PC histogram that silently dropped up to 45% of the instructions (identity hash on the
PC's low bits, 64-slot probe: ROM code arriving behind SDRAM code was lost). With a
multiplicative hash and zero dropped, three loops showed up and were folded.

| Title | Change | Rig, host insn/frame | Device, drawn fps (approx) |
| --- | --- | ---: | ---: |
| Virtua Fighter | master's 27-insn command-dispatcher idle loop folded | -40.9% (2000 frames) | 10.75 -> 12.5 / 12.55 / 12.7; frames in first 140 s 2,105 -> 2,755 |
| Kolibri | SDRAM poll fastloop accepts a body in cart ROM | -28.3% (300 frames) | intro 14.0 -> 17.85; attract gameplay 13.0 -> 13.3 |
| Chaotix | word copy loop executed natively | -3.6% (2600 frames) | 17.9 -> 19.1 / 19.2 |

Rig framebuffer and sound hashes identical in every A/B; Doom and After Burner unchanged.
Device windows are 20 s, shell-timed, after a 120 s cold start with no input; a faster build
is further into the demo at the same wall time, so the cumulative frame count is the fairer
figure. Installed build: `testbed-full-20260906-1924-81-g08fb76be`. `/CONFIG` identical to the
original after the session.

Still open, measured with the fixed histogram in the gameplay window: Kolibri slave PCM mixer
(about 26% of guest instructions), Chaotix scaler loops at 0x060019xx / 0x060021xx (about 27%),
After Burner blit at 0x06006axx (about 28%) and a table search at 0x060065c8 (9.5%). All real
work: a native routine each, with the differential proof the existing natives used.

Traps met: a fold inlined into `gnw_sh2_fastloop` cost +14% with zero folds (needs `noinline`);
a 900-byte helper fits in neither ITCM nor RAM_EMU; `itl` handoff of the hypothesis came back
"rejected" because of the histogram bug, so check `dropped` before believing a top list.
