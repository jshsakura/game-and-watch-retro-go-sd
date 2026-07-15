# N-SPC sound HLE, wired into the live emulator

`tools/nspc_hle` proved the native player renders foreign N-SPC music from an
ARAM snapshot. This wires it into the RUNNING emulator: boot LLE (the real
SPC700 does the IPL upload), auto-detect the driver (survey signatures,
chOK>=6 twice, 60 frames apart), swap to the native player, and from then on
`apu_run()` advances the 500 Hz sequencer tick + `dsp_cycle` instead of SPC700
opcodes. The game's $2140-43 traffic talks to the native engine.

```
bash tools/nspc_audio_wire/build.sh          # host harness -> /tmp/nspc_wire_build/wire_host
WIRE=0 wire_host <rom> [frames]              # pure-LLE reference
wire_host <rom> [frames]                     # wired (WIRE_WAV=/tmp/x.wav dumps audio)
bash tools/nspc_audio_wire/run_snes_wire.sh <rom> [frames]   # hard-float M7 rig
```

## Measured (M7 rig, hard-float, Zelda ALttP gameplay windows, 340 MHz)

| | emu | apu | total | fps |
|---|---|---|---|---|
| stock LLE (PPU commit in) | 6.31M | 0.60M | 6.90M | 49.2 |
| **wired (std N-SPC native)** | **5.73M** | **0.195M** | **5.93M** | **57.3 (+16%)** |

−0.98M/frame: SPC700 opcodes (0.37M) plus the per-APU-cycle `apu_cycle`
bookkeeping (timers/dispatch at 1.024 MHz -> replaced by per-sample stepping,
32× fewer iterations). **STATEHASH and every per-window fb hash identical to
stock** — the game never observed a divergent port value in this run.

## What works / what doesn't (the honest boundary)

- **std-variant protocol (SM/ALttP lineage): works.** Zelda title music plays
  natively; scene milestones identical; 3000-frame no-hang.
- **Konami (GD3): sequence dialect solved, live PORT PROTOCOL not.** TMNT
  hangs at boot on stateful driver responses that instant-acks cannot fake;
  Gradius III proceeds but near-silent. **The swap is gated to std/YI** —
  GD3/SMW/Tose/Intelli stay LLE (`WIRE_ALL=1` overrides for research).
- Foreign SFX protocols (ports 1-3) are instant-acked, not played — music-only.
- **Open issue:** ALttP's title theme one-shots natively (~19 s) where LLE
  loops forever. The stop trace shows the engine taking the `t==0` end path;
  the standard `$00nn (nn>=0x80)` phrase-jump patch (build.sh) did not change
  it — the loop lives in a per-revision phrase/vcmd difference still to be
  pinned. Bounded, not architectural.

## Protocol nuances that cost hours (keep for the shipped integration)

1. **Idle-zero**: ALttP's NMI mailbox writes 00 to port0 when idle; SM's
   engine reads a 0 that differs from the current song as a STOP command.
   The wire drops idle-zero writes (games stop via 0xf0/0xf1).
2. **Song resume at swap**: the port0 value seen during boot is IPL upload
   counter traffic, not a command; and the handshake clears the port after
   ack. Recover the current song from the driver's own out-port echo, with a
   pc<0xFFC0-filtered write sniff as fallback.
3. **Re-upload risk**: games that re-upload banks mid-run (driver transfer
   command) would break the native player — production needs unswap-to-LLE
   (the frozen SPC700 state remains valid) and re-detection. Not yet needed by
   the tested games, unimplemented.

Files: `wire.c/.h` (dispatch + detection + swap), `host_main.c`,
`build.sh` (generated-copy discipline: player dialect pipeline from nspc_hle
+ `apu_run`->`apu_run_lle` rename in a copied apu.c; submodule untouched),
`run_snes_wire.sh` (+ generated `rig_snes_wire.c`).
