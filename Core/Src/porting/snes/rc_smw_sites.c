/* rc_smw_sites.c — SMW per-ROM static recompilation, XIP compilation unit.
 *
 * Ships as rc_smw.xip on SD, cached into QSPI XIP at runtime (sentinel
 * RCSMW_CODE 0xD1D0xxxx, same trick as sm.xip). Contains:
 *
 *   1. cpu_copy.c  — the interpreter's own static helpers (cpu_read,
 *                    cpu_write, cpu_setZN, cyclesPerOpcode, ...). Included
 *                    wholesale because every site calls them and they are
 *                    `static` in the overlay's cpu.c — invisible to other
 *                    TUs, so the XIP blob MUST carry its own copy. Both
 *                    copies are file-scope local; no symbol clash.
 *   2. rc_fetch8 / rc_adr* — the rc constant-folded fetch + addressing
 *                    helpers (from tools/sfc_recomp/rc_core.c). These are
 *                    NOT in the device's cpu.c.
 *   3. rc_sites.inc — 8371 native site functions + rc_fns[] + rc_addrs[].
 *   4. rc_smw_header — a discovery struct at blob offset 0 whose sentinel
 *                    pointers the cache-relocation pass patches to the
 *                    real flash addresses of rc_fns/rc_addrs.
 *
 * Compiled with the SAME flags and snes_redefines as the SNES overlay so
 * that externs (snes_cpuRead, HookedFunctionRts, ...) resolve to the
 * overlay's renamed gsnes__* definitions via linker-generated XIP->RAM
 * veneers. cpu_copy.c's own globals (cpu_init, cpu_reset, ...) are #define-
 * renamed to unique names before inclusion so they cannot clash with the
 * overlay's gsnes__* versions; they are dead code (the sites never call
 * them) and --gc-sections discards them. NO -DSNES_SPIN_SKIP: rc replaces
 * the interpreter, so the spin hooks are not needed in the XIP path.
 *
 * Activation (main_snes.c rc_smw_activate): CRC32-detect SMW, cache the
 * blob, read the header, rc_dispatch_init(rc_addrs, nsites, rc_fns). The
 * overlay's cpu_runOpcode fast path then dispatches to sites instead of
 * interpreting. Rig-measured: -42.3% insn/frame on SMW, bit-identical
 * state hash. */
#include <stdint.h>
#include <stdbool.h>

/* cpu_copy.c is a byte-for-byte copy of external/sm/src/snes/cpu.c (with
 * cpu_runOpcode renamed to rc_orig_runOpcode so this TU does not export a
 * competing cpu_runOpcode). It carries the static helpers the sites call.
 * Its relative includes (../types.h etc.) resolve via -Iexternal/sm/src/snes
 * in the compile rule. */
#define cpu_init       rc_smw_cpu_init_unused
#define cpu_free       rc_smw_cpu_free_unused
#define cpu_reset      rc_smw_cpu_reset_unused
#define cpu_saveload   rc_smw_cpu_saveload_unused
#define cpu_getFlags   rc_smw_cpu_getFlags_unused
#define cpu_setFlags   rc_smw_cpu_setFlags_unused
#define DumpCpuHistory rc_smw_DumpCpuHistory_unused
#define pc_hist        rc_smw_pc_hist_unused
#define pc_hist_ctr    rc_smw_pc_hist_ctr_unused
#define pc_bp          rc_smw_pc_bp_unused

#include "cpu_copy.c"

#undef cpu_init
#undef cpu_free
#undef cpu_reset
#undef cpu_saveload
#undef cpu_getFlags
#undef cpu_setFlags
#undef DumpCpuHistory
#undef pc_hist
#undef pc_hist_ctr
#undef pc_bp

/* The sites call cpu_getFlags/cpu_setFlags (global in cpu.c). cpu_copy.c's
 * copies were renamed above; re-declare the originals so the compiler knows
 * their signature. snes_redefines (applied by the compile rule) renames these
 * references to gsnes__cpu_getFlags/gsnes__cpu_setFlags, resolving via
 * linker veneers to the SNES overlay's definitions. */
uint8_t cpu_getFlags(Cpu *cpu);
void cpu_setFlags(Cpu *cpu, uint8_t val);

/* ---- rc constant-folded fetch + addressing (from rc_core.c, verbatim) ----
 * Each mirrors cpu.c's interpreter path exactly, with the operand FETCH
 * replaced by its known ROM constant plus the fetch's exact bus side
 * effects (cpuMemOps++, cpuCyclesLeft += 8, pc++). Data accesses still go
 * through the real bus (cpu_read/cpu_write). */

static inline uint8_t rc_fetch8(Cpu *cpu, uint8_t v) {
  Snes *s = (Snes *)cpu->mem;
  s->cpuMemOps++;
  s->cpuCyclesLeft += 8;
  cpu->pc++;
  return v;
}
static inline uint16_t rc_fetch16(Cpu *cpu, uint16_t v) {
  Snes *s = (Snes *)cpu->mem;
  s->cpuMemOps += 2;
  s->cpuCyclesLeft += 16;
  cpu->pc += 2;
  return v;
}

static inline uint32_t rc_adrDp(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  if (cpu->dp & 0xff) cpu->cyclesUsed++;
  *low = (cpu->dp + adr) & 0xffff;
  return (cpu->dp + adr + 1) & 0xffff;
}
static inline uint32_t rc_adrDpx(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  if (cpu->dp & 0xff) cpu->cyclesUsed++;
  *low = (cpu->dp + adr + cpu->x) & 0xffff;
  return (cpu->dp + adr + cpu->x + 1) & 0xffff;
}
static inline uint32_t rc_adrDpy(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  if (cpu->dp & 0xff) cpu->cyclesUsed++;
  *low = (cpu->dp + adr + cpu->y) & 0xffff;
  return (cpu->dp + adr + cpu->y + 1) & 0xffff;
}
static inline uint32_t rc_adrIdp(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  if (cpu->dp & 0xff) cpu->cyclesUsed++;
  uint16_t pointer = cpu_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  *low = (cpu->db << 16) + pointer;
  return ((cpu->db << 16) + pointer + 1) & 0xffffff;
}
static inline uint32_t rc_adrIdy(Cpu *cpu, uint32_t *low, bool write, uint8_t adr) {
  rc_fetch8(cpu, adr);
  if (cpu->dp & 0xff) cpu->cyclesUsed++;
  uint16_t pointer = cpu_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  if (write && (!cpu->xf || ((pointer >> 8) != ((pointer + cpu->y) >> 8)))) cpu->cyclesUsed++;
  *low = ((cpu->db << 16) + pointer + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + pointer + cpu->y + 1) & 0xffffff;
}
static inline uint32_t rc_adrIdl(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  if (cpu->dp & 0xff) cpu->cyclesUsed++;
  uint32_t pointer = cpu_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  pointer |= cpu_read(cpu, (cpu->dp + adr + 2) & 0xffff) << 16;
  *low = pointer;
  return (pointer + 1) & 0xffffff;
}
static inline uint32_t rc_adrIly(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  if (cpu->dp & 0xff) cpu->cyclesUsed++;
  uint32_t pointer = cpu_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff);
  pointer |= cpu_read(cpu, (cpu->dp + adr + 2) & 0xffff) << 16;
  *low = (pointer + cpu->y) & 0xffffff;
  return (pointer + cpu->y + 1) & 0xffffff;
}
static inline uint32_t rc_adrAbs(Cpu *cpu, uint32_t *low, uint16_t adr) {
  rc_fetch16(cpu, adr);
  *low = (cpu->db << 16) + adr;
  return ((cpu->db << 16) + adr + 1) & 0xffffff;
}
static inline uint32_t rc_adrAbx(Cpu *cpu, uint32_t *low, bool write, uint16_t adr) {
  rc_fetch16(cpu, adr);
  if (write && (!cpu->xf || ((adr >> 8) != ((adr + cpu->x) >> 8)))) cpu->cyclesUsed++;
  *low = ((cpu->db << 16) + adr + cpu->x) & 0xffffff;
  return ((cpu->db << 16) + adr + cpu->x + 1) & 0xffffff;
}
static inline uint32_t rc_adrAby(Cpu *cpu, uint32_t *low, bool write, uint16_t adr) {
  rc_fetch16(cpu, adr);
  if (write && (!cpu->xf || ((adr >> 8) != ((adr + cpu->y) >> 8)))) cpu->cyclesUsed++;
  *low = ((cpu->db << 16) + adr + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + adr + cpu->y + 1) & 0xffffff;
}
static inline uint32_t rc_adrAbl(Cpu *cpu, uint32_t *low, uint32_t adr) {
  rc_fetch16(cpu, (uint16_t)adr);
  rc_fetch8(cpu, (uint8_t)(adr >> 16));
  *low = adr;
  return (adr + 1) & 0xffffff;
}
static inline uint32_t rc_adrAlx(Cpu *cpu, uint32_t *low, uint32_t adr) {
  rc_fetch16(cpu, (uint16_t)adr);
  rc_fetch8(cpu, (uint8_t)(adr >> 16));
  *low = (adr + cpu->x) & 0xffffff;
  return (adr + cpu->x + 1) & 0xffffff;
}

/* ---- the 8371 site functions + rc_fns[] + rc_addrs[] ---- */
#include "rc_sites.inc"

/* ---- runtime discovery header (placed at blob offset 0 by the linker) ----
 * main_snes.c reads this after caching to find rc_fns/rc_addrs. The
 * sentinel pointer values are patched by the cache-relocation pass
 * (RcSmwRelocateXip, same scan as PatchSmSentinels). */
#define RC_SMW_MAGIC  0x4D534352u   /* 'RCSM' little-endian */

typedef void (*rc_smw_fn_t)(Cpu *);

__attribute__((section(".rc_smw_hdr"), used))
const struct rc_smw_header {
  uint32_t magic;            /* RC_SMW_MAGIC */
  uint32_t nsites;           /* RC_NSITES */
  const uint32_t *addrs;     /* sentinel addr of rc_addrs[] — patched at cache time */
  const rc_smw_fn_t *fns;    /* sentinel addr of rc_fns[] — patched at cache time */
} rc_smw_header = {
  RC_SMW_MAGIC,
  RC_NSITES,
  rc_addrs,
  rc_fns,
};
