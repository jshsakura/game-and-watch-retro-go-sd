/* Where does the SPC700 spend its opcodes?
 *
 * Linked with -Wl,--wrap=spc_runOpcode, so every opcode the APU runs passes
 * through here on its way to the real interpreter. Nothing in external/sm is
 * touched. The question this answers: is the guest's sound driver sitting in a
 * tight wait loop (an idle-skip lever) or actually working (no lever)?
 *
 * Prints, at exit: opcodes and SPC cycles per PC, the hottest sites, and the
 * bytes around the hottest one so the loop can be disassembled by hand — what
 * it polls decides whether an idle-skip is even expressible.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "src/snes/spc.h"
#include "src/snes/apu.h"

#define PC_SPACE 65536
#define TOP_N    12
#define WINDOW   8          /* a PC seen again within 8 opcodes = a tight loop */

static uint32_t *pc_ops;
static uint32_t *pc_cycles;
static uint64_t  total_ops, total_cycles, backward_hits;
static uint16_t  recent[WINDOW];
static int       recent_at;
static Spc      *seen_spc;
static int       skip_on;          /* SPC_SKIP=1 */
static uint64_t  skipped_ops;

int __real_spc_runOpcode(Spc *spc);

static void spc_probe_report(void)
{
  if (!pc_ops || !total_ops) return;

  uint32_t bestpc[TOP_N] = {0};
  uint32_t best[TOP_N] = {0};
  for (uint32_t pc = 0; pc < PC_SPACE; pc++) {
    uint32_t n = pc_ops[pc];
    if (!n) continue;
    for (int i = 0; i < TOP_N; i++) {
      if (n > best[i]) {
        memmove(&best[i + 1], &best[i], (TOP_N - i - 1) * sizeof best[0]);
        memmove(&bestpc[i + 1], &bestpc[i], (TOP_N - i - 1) * sizeof bestpc[0]);
        best[i] = n; bestpc[i] = pc;
        break;
      }
    }
  }

  uint64_t top_ops = 0, top_cyc = 0;
  for (int i = 0; i < TOP_N; i++) { top_ops += best[i]; top_cyc += pc_cycles[bestpc[i]]; }

  if (skip_on)
    fprintf(stderr, "[spc] SKIP on: %llu opcodes never dispatched (%.1f%% of what the "
                    "reference runs)\n", (unsigned long long)skipped_ops,
            100.0 * (double)skipped_ops / (double)(skipped_ops + total_ops));

  fprintf(stderr, "[spc] opcodes=%llu cycles=%llu  top%d: %.1f%% of ops, %.1f%% of cycles"
                  "  revisit<=%d=%.1f%%\n",
          (unsigned long long)total_ops, (unsigned long long)total_cycles, TOP_N,
          100.0 * (double)top_ops / (double)total_ops,
          100.0 * (double)top_cyc / (double)total_cycles,
          WINDOW, 100.0 * (double)backward_hits / (double)total_ops);

  for (int i = 0; i < TOP_N && best[i]; i++)
    fprintf(stderr, "[spc]   $%04x ops=%9u %5.2f%%  cyc=%9u %5.2f%%\n",
            bestpc[i], best[i], 100.0 * (double)best[i] / (double)total_ops,
            pc_cycles[bestpc[i]], 100.0 * (double)pc_cycles[bestpc[i]] / (double)total_cycles);

  if (seen_spc && seen_spc->apu && best[0]) {
    uint32_t base = bestpc[0] > 8 ? bestpc[0] - 8 : 0;
    fprintf(stderr, "[spc] aram $%04x:", base);
    for (int i = 0; i < 40; i++)
      fprintf(stderr, "%s%02x", i == 8 ? " >" : " ", seen_spc->apu->ram[(base + i) & 0xffff]);
    fprintf(stderr, "\n");
  }
}

/* --- the prototype lever -------------------------------------------------
 * The N-SPC driver's main wait is  EC FD 00  MOV A,$00FD ; F0 FB  BEQ -5 :
 * read timer 0's counter (the read clears it) and loop while it is zero. Every
 * iteration before the counter ticks reads 0 and clears an already-zero
 * counter — no observable state changes, only cycles pass. So charge those
 * cycles in one step instead of dispatching the opcodes.
 *
 * Exactness: we stop one whole iteration short of the tick, so the reference's
 * exit iteration is still interpreted, at the same cycle. The timers and the
 * 32-cycle DSP tick are advanced by apu_run's existing closed-form bulk update
 * over the skipped span, so the audio is not merely close, it is identical —
 * which the harness's audio hash proves.
 */
static int spc_try_skip(Spc *spc)
{
  Apu *apu = spc->apu;
  uint16_t pc = spc->pc;
  const uint8_t *r = apu->ram;

  if (r[pc] != 0xec || r[(uint16_t)(pc + 1)] != 0xfd || r[(uint16_t)(pc + 2)] != 0x00 ||
      r[(uint16_t)(pc + 3)] != 0xf0 || r[(uint16_t)(pc + 4)] != 0xfb)
    return 0;

  Timer *t = &apu->timer[0];
  if (!t->enabled || t->counter != 0 || t->target == 0 || t->divider >= t->target)
    return 0;                                  /* only the plain, common case */

  int ticks  = t->target - t->divider;         /* timer ticks until counter++ */
  int to_inc = (int)t->cycles + (ticks - 1) * 128;
  int cycles = ((to_inc - 1) / 8) * 8;         /* whole 8-cycle iterations before it */
  if (cycles > 248) cycles = 248;              /* apu->cpuCyclesLeft is a uint8_t */
  /* RED arms: overshoot AFTER the clamp, or the clamp swallows the error and the
   * "failing" run is byte-identical to the passing one -- which it was, first try. */
  if (skip_on == 2) cycles += 8;               /* one iteration too far */
  if (skip_on == 3) cycles += 128;             /* a whole timer tick too far */
  return cycles >= 8 ? cycles : 0;
}

int __wrap_spc_runOpcode(Spc *spc)
{
  if (!pc_ops) {
    pc_ops = calloc(PC_SPACE, sizeof *pc_ops);
    pc_cycles = calloc(PC_SPACE, sizeof *pc_cycles);
    seen_spc = spc;
    /* && would fold every arm to 1 -- which it did, and both RED arms came back
     * GREEN because they were running the exact code. Take the value. */
    { const char *e = getenv("SPC_SKIP"); skip_on = e ? atoi(e) : 0; }
    atexit(spc_probe_report);
  }

  uint16_t pc = spc->pc;

  if (skip_on) {
    int jump = spc_try_skip(spc);
    if (jump) {
      skipped_ops += jump / 4;    /* two opcodes per 8-cycle iteration */
      total_cycles += (uint64_t)jump;
      return jump;
    }
  }

  int cycles = __real_spc_runOpcode(spc);

  pc_ops[pc]++;
  pc_cycles[pc] += (uint32_t)(cycles > 0 ? cycles : 0);
  total_ops++;
  total_cycles += (uint64_t)(cycles > 0 ? cycles : 0);

  for (int i = 0; i < WINDOW; i++)
    if (recent[i] == pc) { backward_hits++; break; }
  recent[recent_at] = pc;
  recent_at = (recent_at + 1) % WINDOW;

  return cycles;
}
