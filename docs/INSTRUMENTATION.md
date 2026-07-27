# Re-arming the instrumentation

Every diagnostic in this tree is flag-gated and compiles away. The shipping
build carries none of it — verified on the release build with
`arm-none-eabi-nm build/gw_retro_go.elf | grep -c snes_prof` returning 0.

So none of it was deleted. Deleting it would throw away the measurement
infrastructure that closed the performance axes, and re-deriving it costs far
more than the zero bytes it occupies when off. This file is the index for
turning it back on.

## SNES

| flag | what it gives | cost when ON |
|---|---|---|
| `SNES_DEVICE_PROFILE=1` | `/snes_dwt.txt` — 3-ledger frame profile: per-phase ACTIVE cycles, an exclusive PPU/APU/CPU/scheduler split, sleep-safe wall clock and audio deadlines, the spin-skip breakeven, and the audio-stretcher state | **~8.5% of ACTIVE.** The `cpu_only` bracket runs per opcode (12,833×/frame). Never size an fps target against a build carrying it. |
| `SNES_PROF_SKIP_FRAMES=N` | which 64 frames get measured; default 600 so the window lands in gameplay | — |
| `SNES_LOAD_DIAG=1` | APU cost split via generated `apu.c`/`dsp.c`. Mutually exclusive with the profiler | |
| `SNES_THUMB2_CPU` / `SNES_THUMB2_SPC` | the native 65816 / SPC700 engines. Both default ON | |

```sh
make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
     DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1 \
     ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1 SNES_DEVICE_PROFILE=1
```

Cut that as its own tag. Do not leave it on the line people play.

## 32X

`MD32X_DEVICE_PROFILE=1` → `/32x_dwt.txt`. Its probes are not per-opcode, so it
is cheaper than the SNES one, but the same rule applies.

## Host rigs

`tools/m7_qemu_rig/` runs the cores on QEMU's Cortex-M7 with `-icount shift=0`,
so a timer delta is an executed-instruction count.

- `run_snes_hf.sh` — **hard float (`-mfpu=fpv5-d16`), matches the device.**
- `run_snes.sh` — soft float. Fine for instruction counts that do not involve
  floating point, and **actively misleading for anything that does**: the
  scheduler's `double` accumulator looks like a −27.95% win here and measures
  +0.93% *slower* on hard float, because the M7 does double in hardware.
- `run_snes_spin.sh` — the spin-skip arm; gate is STATEHASH against the stock run.
- `tools/dsp1_harness/dsp1_run.sh` — DSP-1 protocol sweep under ASan/alignment
  with the device's defines.

Measured QEMU→device transfer coefficient: **0.64** (SPC700 Thumb-2 predicted
−5.8% on the rig, delivered −3.71% on hardware). Useful for sizing, not for
deciding.

## Two rules these cost us to learn

1. **Profile the scene you care about.** Three dumps answered a gameplay
   question with title-screen numbers. The spin learner looked like a 16.1%
   parasite because it replays 0.00% at boot and 53.3% in play.
2. **Check workload identity before trusting an A/B.** Scene variance alone
   moved fps 3.4% between runs of the same code. The SPC700 A/B was only
   believable because `cpu_calls/frame` was 12,833 in both arms.
