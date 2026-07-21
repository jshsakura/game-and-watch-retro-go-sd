/* ---- hybrid dispatch (rig variant) ----------------------------------------
 * Same drop-in cpu_runOpcode as tools/sfc_recomp/rc_core.c, with one change:
 * the host's flat 16M-entry site map is 32 MB — the mps2-an500 has 16 MB total.
 * A 256-entry bank table with per-bank 64K maps allocated only for banks that
 * contain sites (Zelda: 7 banks = 896 KB) is what a device build would do too;
 * dispatch costs one extra load per opcode, which the measurement then includes
 * honestly. Preamble is byte-for-byte the interpreter's (see cpu_copy.c). */

uint64_t g_rc_native, g_rc_interp;
static uint16_t *rc_banks[256];
static int rc_ready;

static void rc_init(void) {
  for (uint32_t i = 0; i < RC_NSITES; i++) {
    uint8_t b = rc_addrs[i] >> 16;
    if (!rc_banks[b]) {
      rc_banks[b] = calloc(0x10000, sizeof(uint16_t));
      if (!rc_banks[b]) { printf("rc: OOM\n"); exit(1); }
    }
    rc_banks[b][rc_addrs[i] & 0xffff] = (uint16_t)(i + 1);
  }
  rc_ready = 1;
}

int cpu_runOpcode(Cpu *cpu) {
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
  if (!rc_ready) rc_init();
  uint16_t *bm = rc_banks[cpu->k];
  uint16_t id = bm ? bm[cpu->pc] : 0;
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
