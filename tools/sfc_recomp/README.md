# 65816→C static translator (PoC)

Translates a ROM's executed 65816 code to C at build time and runs it hybrid
(native per-site functions + the verbatim interpreter as fallback), gated on
**bit-identical state+audio hashes** vs the pure interpreter.

```bash
bash tools/sfc_recomp/build.sh "<rom.smc>" [frames]
```

Pipeline: dump run (`-DSNES_PC_HISTOGRAM` harness records every executed opcode
site + the live expanded cart image) → `translate.py` (parses `cpu.c`'s own
switch, splices each opcode body verbatim, constant-folds opcode/operand fetches
with their exact bus side effects: `cpuMemOps++`, `cpuCyclesLeft += 8`, `pc++`)
→ `rc_core.c` (one TU: interpreter copy + folded addressing helpers + generated
sites + drop-in `cpu_runOpcode` wrapper).

Design constraint: **one opcode per call.** The event loop charges cycles and
fires NMI/IRQ/DMA between opcodes; block fusion would shift timing and break the
hash gate. The wrapper replicates the interpreter's interrupt/WAI preamble
byte-for-byte, then dispatches PC→site via a 16M-entry map; unmapped PCs (code
the dump never saw, WRAM stubs) fall back to the interpreter mid-stream.

Results (0715): Zelda 4,355 sites / SMW 8,371 sites translated; state+audio
hashes bit-identical (1500f, 3000f, and a forced half-map native/interp
interleave); runtime native coverage 100% / 99.97%; CPU bus calls −77%
(36.4M→8.45M per 1000 frames); host whole-frame speedup 1.17–1.20× (host hides
in-order fetch costs — M7 rig verdict pending).
