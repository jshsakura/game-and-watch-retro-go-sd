# crash_decode — generic on-device fault decoder

The BSOD gives you a fault title, a `PC` and an `LR`, and nothing else
(`SCB->SHCSR` is never written, so every fault escalates to a bare "Hardfault";
`CFSR/BFAR/ABFSR` are not printed). Worse, `PC`/`LR` are raw addresses that a
plain `addr2line` **cannot** resolve, because a crash lands in one of three
spaces:

| space | range | why raw addr2line fails |
|-------|-------|-------------------------|
| resident intflash | `0x08xxxxxx` | — (works directly) |
| overlay RAM | `0x24025800`–`0x24100000` | every core's overlay is linked at the **same** VMA, so the address **aliases** — you get whichever core's symbol is found first |
| runtime XIP flash | `0x9xxxxxxx` | cold code is linked at a sentinel base (`SEGACD_CODE=0xDEC80000`, `SM=0xDEAD0000`, `GBA=0xDEC00000`, `PICO8=0xBEEF0000`) and copied to a **runtime** flash address at boot; addr2line only knows the sentinel |

This tool does the address arithmetic the handoff notes describe, automatically.

## Use

Paste the whole BSOD (mojibake and line-wraps are fine) on stdin:

```
tools/crash_decode/decode.py build/gw_retro_go.elf < paste.txt
```

It auto-extracts `PC=`, `LR=`, the `xip blob at 0x..` base, the active core
name, and `CFSR=` if present. Override any of them:

```
tools/crash_decode/decode.py build/gw_retro_go.elf \
    --pc 0 --lr 0x926a208b --xip-base 0x926a2000 --core segacd
```

### What it resolves

- **XIP** `LR` → un-relocates `runtime - xip_base + CODE_BASE`, then symbolises.
  (Validated: `0x926a208b` → `0xDEC8008b` → `m68ki_read_imm_32`, m68kcpu.h:851.)
- **NULL** `PC`/`LR` → flags an indirect call/deref through an uninitialised
  pointer; the other register names the faulting call site.
- **Overlay** address → symbolises, then **warns if the symbol belongs to a
  different core than the active one** (the alias trap) and tells you to resolve
  against `build/<core>/*.o`. Manual fallback: `arm-none-eabi-objdump -d
  build/<core>/<file>.o`.
- **CFSR** value → decodes every fault bit (INVSTATE = branch to a NULL/garbage
  pointer, UNALIGNED = M7 64-bit STRD/LDRD to a non-word-aligned address,
  IMPRECISERR = buffered store, read ABFSR, …).

The ELF **must be the exact build that crashed** (addresses shift between
builds). Use the `gw_retro_go.elf` from the same release as the firmware on the
SD card.

## Phase 2 (device side, not done)

To close the loop so the device itself emits everything this tool needs:

1. `SCB->SHCSR |= USGFAULTENA|BUSFAULTENA|MEMFAULTENA` in `main()` → the title
   becomes Usagefault/Busfault/Memfault instead of a bare Hardfault (the
   per-fault handlers already exist in `stm32h7xx_it.c`).
2. Print `CFSR`, `HFSR`, `BFAR`, `MMFAR`, `ABFSR` in the BSOD.
3. Optionally have the fault handler un-relocate XIP addresses itself, from a
   small per-core `{code_base, xip_runtime_base}` registry, so the printed
   address maps straight to the ELF.

Watch the intflash budget (~44 B free on the canonical docker build) — keep the
added format strings compact, or free space first.
