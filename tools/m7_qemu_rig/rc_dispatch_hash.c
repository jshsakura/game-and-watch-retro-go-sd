/* ---- hybrid dispatch (per-bank hash table, LF~0.5) -------------------------
 * Device-realistic dispatch: per-bank open-addressing hash tables with linear
 * probing.  Replaces the flat 16M-entry map with a compact structure that fits
 * DTCM.  Lookup is O(1) average (~1.5 probes at LF 0.5) vs binary search's
 * O(log N) ~12 iterations.
 *
 * Total footprint for SMW (8371 sites, 7 banks): ~60 KB.  Fits DTCM (81 KB heap).
 *
 * Hash: Knuth multiplicative (pc * golden_ratio >> shift).  Per-bank table
 * sized to next pow2 above 2*site_count.  Linear probing on collision.
 *
 * Preamble is byte-for-byte the interpreter's (see cpu_copy.c). */

uint64_t g_rc_native, g_rc_interp;


static rc_entry_t *rc_hash[256];
static int rc_hash_mask[256];
static int rc_ready;

static void rc_init(void) {
  int counts[256] = {0};
  for (uint32_t i = 0; i < RC_NSITES; i++)
    counts[rc_addrs[i] >> 16]++;
  for (int b = 0; b < 256; b++) {
    if (!counts[b]) continue;
    int sz = 1;
    while (sz < counts[b] * 2) sz <<= 1;  /* LF ~0.5 */
    rc_hash[b] = malloc(sz * sizeof(rc_entry_t));
    memset(rc_hash[b], 0, sz * sizeof(rc_entry_t));
    rc_hash_mask[b] = sz - 1;
  }
  for (uint32_t i = 0; i < RC_NSITES; i++) {
    uint8_t b = rc_addrs[i] >> 16;
    uint16_t pc = rc_addrs[i] & 0xffff;
    uint16_t id = (uint16_t)(i + 1);
    int mask = rc_hash_mask[b];
    uint32_t h = (pc * 2654435761u) >> (32 - __builtin_clz(mask + 1) + 1);
    /* above clz gives log2(sz); simpler: h = (pc * 2654435761u) & mask */
    h = (pc * 2654435761u) & mask;
    while (rc_hash[b][h].id)  /* linear probe (0 = empty) */
      h = (h + 1) & mask;
    rc_hash[b][h].pc = pc;
    rc_hash[b][h].id = id;
  }
  rc_ready = 1;
}

static inline uint16_t rc_lookup(uint8_t bank, uint16_t pc) {
  int mask = rc_hash_mask[bank];
  if (!mask) return 0;
  rc_entry_t *ht = rc_hash[bank];
  uint32_t h = (pc * 2654435761u) & mask;
  for (;;) {
    uint16_t eid = ht[h].id;
    if (!eid) return 0;            /* empty slot → miss */
    if (ht[h].pc == pc) return eid; /* hit */
    h = (h + 1) & mask;            /* linear probe */
  }
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
