# SNES M7 batch cost harness

This runner profiles a SNES ROM collection without flashing the Game & Watch and
without rebuilding the emulator for every cartridge. It builds two Cortex-M7,
hard-float ELFs once, including the device's `SNES_SPIN_SKIP` whitelist/learner
and exact replay loop, then injects each ROM into QEMU PSRAM at run time.

```sh
tools/m7_qemu_rig/run_snes_cost_batch.py /path/to/snes-roms \
  --frames 300 --jobs 4 --csv /tmp/snes-costs.csv
```

The render-on and render-off runs are deterministic. Their `emu` difference is
the PPU line-render/composition cost. `COREHASH` and `AUDIOHASH` must remain
identical between those runs, proving that the subtraction did not change the
emulated machine. The device video path is reproduced in the
rig: 256-pixel line copies into a private 320x240 framebuffer plus the full
320x240 present copy. The CSV also records 65816, SPC700 and DSP instruction
costs, generated DSP samples, average active/echo voices, state/audio hashes,
spin-skip coverage/gate state, timeouts and unsupported loads.

The CSV is append-only and flushed after every ROM. Re-running the same command
skips completed rows only when the ROM SHA-256, reusable-ELF build ID and frame
count all still match, so power loss does not restart a large scan and stale
results cannot turn a new core build green.
Use `--rerun` to replace it. A short 300-frame pass is the library-wide smoke
gate; rebuild with `--frames 1500` for a smaller, representative deep set.

Boundary: this is real ARMv7-M code and device compile flags, but QEMU reports
executed instructions, not STM32H7 cache/XIP stalls. A small fixed device corpus
calibrates that model; the full ROM library remains a host-only automated gate.
