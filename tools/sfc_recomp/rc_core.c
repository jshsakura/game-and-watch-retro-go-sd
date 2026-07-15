/* Hybrid 65816: statically translated per-site native code + the verbatim
 * interpreter as fallback, in ONE translation unit so GCC inlines the
 * interpreter's own helpers into the generated sites.
 *
 * cpu_copy.c is a byte-for-byte copy of external/sm/src/snes/cpu.c with only
 * cpu_runOpcode renamed (this file exports the hybrid one instead). Every
 * addressing-mode/op helper below mirrors its cpu.c original exactly, with the
 * operand FETCH replaced by its known ROM constant plus the fetch's exact bus
 * side effects (snes_cpuRead: cpuMemOps++, cpuCyclesLeft += 8, pc++). Data
 * accesses still go through the real bus. Execution stays one opcode per
 * cpu_runOpcode call — the event loop charges cycles and fires NMI/IRQ/DMA
 * between opcodes, so anything coarser would shift timing and break the
 * bit-identical state-hash gate.
 */
#include "cpu_copy.c"

/* ---- constant-folded fetch + addressing (each mirrors cpu.c exactly) ---- */

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
static inline uint32_t rc_adrIdx(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  if (cpu->dp & 0xff) cpu->cyclesUsed++;
  uint16_t pointer = cpu_readWord(cpu, (cpu->dp + adr + cpu->x) & 0xffff, (cpu->dp + adr + cpu->x + 1) & 0xffff);
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
static inline uint32_t rc_adrSr(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  *low = (cpu->sp + adr) & 0xffff;
  return (cpu->sp + adr + 1) & 0xffff;
}
static inline uint32_t rc_adrIsy(Cpu *cpu, uint32_t *low, uint8_t adr) {
  rc_fetch8(cpu, adr);
  uint16_t pointer = cpu_readWord(cpu, (cpu->sp + adr) & 0xffff, (cpu->sp + adr + 1) & 0xffff);
  *low = ((cpu->db << 16) + pointer + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + pointer + cpu->y + 1) & 0xffffff;
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
static inline uint16_t rc_adrIax(Cpu *cpu, uint16_t adr) {
  rc_fetch16(cpu, adr);
  return cpu_readWord(cpu, (cpu->k << 16) | ((adr + cpu->x) & 0xffff),
                      (cpu->k << 16) | ((adr + cpu->x + 1) & 0xffff));
}

/* ---- generated sites ---- */
#include "rc_sites.inc"

/* ---- hybrid dispatch: drop-in cpu_runOpcode ---- */

uint64_t g_rc_native, g_rc_interp;
static uint16_t *rc_map;   /* 16M entries: site index+1, 0 = interpreter */

static void rc_init(void) {
  rc_map = calloc(1u << 24, sizeof(uint16_t));
  if (!rc_map) { fprintf(stderr, "rc: out of memory\n"); exit(1); }
  for (uint32_t i = 0; i < RC_NSITES; i++)
    rc_map[rc_addrs[i]] = (uint16_t)(i + 1);
}

int cpu_runOpcode(Cpu *cpu) {
  /* preamble: byte-for-byte the interpreter's (see cpu_copy.c) */
  cpu->cyclesUsed = 0;
  if (cpu->stopped) return 1;
  if (cpu->waiting) {
    if (!(cpu->irqWanted || cpu->nmiWanted)) return 1;
    cpu->waiting = false;
  }
  if ((!cpu->i && cpu->irqWanted) || cpu->nmiWanted) {
    cpu->cyclesUsed = 7;
    if (cpu->nmiWanted) {
      cpu->nmiWanted = false;
      cpu_doInterrupt(cpu, false);
    } else {
      cpu_doInterrupt(cpu, true);
    }
  }
  if (!rc_map) rc_init();
  uint16_t id = rc_map[((uint32_t)cpu->k << 16) | cpu->pc];
  if (id) {
    g_rc_native++;
    rc_fns[id - 1](cpu);
    return cpu->cyclesUsed;
  }
  g_rc_interp++;
  uint8_t opcode = cpu_readOpcode(cpu);
  cpu->cyclesUsed = cyclesPerOpcode[opcode];
  cpu_doOpcode(cpu, opcode);
  return cpu->cyclesUsed;
}
