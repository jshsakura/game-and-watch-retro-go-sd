# SNES SMW hot-RC verification handoff

This is a post-build gate. It does not run `make`, `make release`, Docker, or
write firmware. The release owner supplies four QEMU logs and the linked device
artifacts from the exact `RCSMW_HOT=1` candidate.

## 1. Exact production correctness gate

The preferred gate compiles the committed production `rc_smw_sites.c` and
`generated/rc_smw/rc_sites.inc` directly. Its host-only linker seam applies the
same title/code hash checks as `main_snes.c`, proves that SMW activates exactly
270 sites with native hits, and proves that Zelda performs no RC lookup:

```sh
bash tools/gnw_hw_harness/run_snes_rc_hot_correctness.sh 1500
```

Set `KEEP_RC_HOT_LOGS=1` to preserve all four logs in a printed temporary
directory. This gate uses the `tools/sfc_recomp/build.sh` host-harness pattern;
it does not invoke firmware or release builds.

For externally captured QEMU logs, the generic hash-only parser remains
available. Run the same frame count and input configuration in both modes:

Check the four logs:

```sh
python3 tools/gnw_hw_harness/verify_snes_rc_hot.py hashes \
  --baseline-smw-log /tmp/snes-rc-hot-verify/smw.base.log \
  --hot-smw-log /tmp/snes-rc-hot-verify/smw.hot.log \
  --baseline-zelda-log /tmp/snes-rc-hot-verify/zelda.base.log \
  --hot-zelda-log /tmp/snes-rc-hot-verify/zelda.hot.log
```

Both `STATEHASH` and `AUDIOHASH` must be exactly equal per ROM. A macro-only run
that does not link and activate the translated sites is not evidence; use the
exact production gate above for the release decision.

## 2. Device memory and 3. live aliasing

Use the actual map, ELF, and object tree from the release owner's candidate
link. A successful link alone is insufficient: the gate independently checks
the section size/address, SNES image+BSS RAM_EMU total, and the relocated 64 KiB
VRAM symbol.

```sh
python3 tools/gnw_hw_harness/verify_snes_rc_hot.py memory \
  --map build/gw_retro_go.map \
  --elf build/gw_retro_go.elf \
  --linker-script STM32H7B0VBTx_SDCARD.ld

python3 tools/gnw_hw_harness/verify_snes_rc_hot.py aliases \
  --build-dir build \
  --elf build/gw_retro_go.elf
```

The alias gate requires a real ARM `nm`/`objdump`; unlike the generic CI helper,
it fails rather than skipping if the toolchain is unavailable.

## One-shot final decision

```sh
python3 tools/gnw_hw_harness/verify_snes_rc_hot.py all \
  --baseline-smw-log /tmp/snes-rc-hot-verify/smw.base.log \
  --hot-smw-log /tmp/snes-rc-hot-verify/smw.hot.log \
  --baseline-zelda-log /tmp/snes-rc-hot-verify/zelda.base.log \
  --hot-zelda-log /tmp/snes-rc-hot-verify/zelda.hot.log \
  --map build/gw_retro_go.map \
  --elf build/gw_retro_go.elf \
  --build-dir build
```

Any mismatch, missing evidence, skipped alias inspection, empty hot section,
ITCM overflow, RAM_EMU overflow, or misplaced VRAM returns non-zero. Only the
final `PASS snes-rc-hot: requested gates complete` is a go decision.
