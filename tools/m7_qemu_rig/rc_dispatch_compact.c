/* ---- hybrid dispatch (compact per-bank sorted arrays) ----------------------
 * Device-realistic dispatch: per-bank sorted (pc,id) arrays + binary search,
 * replacing the host's flat 16M-entry map (32 MB) and the rig's flat-bank
 * variant (128 KB/bank).  Total footprint for SMW (8371 sites, 7 banks):
 * ~33 KB of (pc,id) pairs + 256-entry bank pointer table.  Fits DTCM.
 *
 * Lookup cost: log2(~max_bank_size) iterations.  Bank 0x00 has ~3700 sites
 * → 12 iterations.  Each iteration: 1 load + 1 compare + 1 branch ≈ 3 cycles
 * in DTCM.  Total ~36 cycles/dispatch vs 2 cycles for flat lookup — but at
 * ~14K dispatches/frame the overhead is ~500K cycles ≈ 0.1% of frame budget.
 *
 * Preamble is byte-for-byte the interpreter's (see cpu_copy.c). */

uint64_t g_rc_native, g_rc_interp;

typedef struct { uint16_t pc; uint16_t id; } rc_entry_t;
static rc_entry_t *rc_bank_sorted[256];
static int rc_bank_sizes[256];
static int rc_ready;

static int rc_cmp(const void *a, const void *b) {
  uint16_t pa = ((const rc_entry_t*)a)->pc;
  uint16_t pb = ((const rc_entry_t*)b)->pc;
  return (pa > pb) - (pa < pb);
}

static void rc_init(void) {
  int counts[256] = {0};
  for (uint32_t i = 0; i < RC_NSITES; i++)
    counts[rc_addrs[i] >> 16]++;
  for (int b = 0; b < 256; b++) {
    if (!counts[b]) continue;
    rc_bank_sorted[b] = malloc(counts[b] * sizeof(rc_entry_t));
  }
  for (uint32_t i = 0; i < RC_NSITES; i++) {
    uint8_t b = rc_addrs[i] >> 16;
    int n = rc_bank_sizes[b];
    rc_bank_sorted[b][n].pc = (uint16_t)(rc_addrs[i] & 0xffff);
    rc_bank_sorted[b][n].id = (uint16_t)(i + 1);
    rc_bank_sizes[b] = n + 1;
  }
  for (int b = 0; b < 256; b++) {
    if (!rc_bank_sizes[b]) continue;
    qsort(rc_bank_sorted[b], rc_bank_sizes[b], sizeof(rc_entry_t), rc_cmp);
  }
  rc_ready = 1;
}

static inline uint16_t rc_lookup(uint8_t bank, uint16_t pc) {
  int n = rc_bank_sizes[bank];
  if (!n) return 0;
  rc_entry_t *arr = rc_bank_sorted[bank];
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (arr[mid].pc == pc) return arr[mid].id;
    if (arr[mid].pc < pc) lo = mid + 1;
    else hi = mid - 1;
  }
  return 0;
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
  uint16_t id = rc_lookup(cpu->k, cpu->pc);
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
