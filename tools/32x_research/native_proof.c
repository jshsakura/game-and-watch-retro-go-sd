/* Cortex-M7/QEMU proof. Includes generated direct calculations, not op handlers. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "generated.h"

extern void proof_bind_rom(const uint16_t *, unsigned int);
extern void rig_timer_init(void);
extern uint32_t rig_timer_now(void);
extern uint32_t rig_calibrate(uint32_t);

static uint32_t random_state = 0x327ac001;
static uint32_t rnd(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
static void seed(SH2 *s, uint32_t pc, unsigned int sample)
{
    static const uint32_t edge[] = {0, 1, 0xffffffff, 0x7fffffff,
                                  0x80000000, 0x80000001, 0x0000ffff, 0xffff0000};
    for (int r = 0; r < 16; r++)
        s->r[r] = sample < 16 ? edge[(sample/2+r) & 7] : rnd();
    s->pc = pc;
    s->ppc = rnd();
    s->pr = rnd(); s->gbr = rnd(); s->vbr = rnd();
    s->mach = rnd(); s->macl = rnd();
    s->sr = rnd() & 0x3f3;
    s->t_flag = sample & 1;
    s->ea = rnd();
    s->delay = 0; s->test_irq = 0;
    s->icount = -77;
    s->is_slave = sample & 1;
    s->p_sdram = (void *)proof_rom; /* read-only instruction fixture, no bus writes allowed */
}
static int fail_state(const char *stage, unsigned int index, unsigned int sample,
                      const SH2 *a, const SH2 *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (unsigned int i = 0; i < sizeof(*a); i++) if (x[i] != y[i]) {
        printf("FAIL stage=%s block=%u sample=%u offset=%u baseline=%02x native=%02x\n",
               stage, index, sample, i, x[i], y[i]);
        break;
    }
    return 1;
}
int main(void)
{
    static SH2 base, candidate, before;
    unsigned int comparisons = 0, rejects = 0;
    const unsigned int count = sizeof(blocks)/sizeof(blocks[0]);
    proof_bind_rom(proof_rom, sizeof(proof_rom));
    rig_timer_init();
    uint32_t calibration = rig_calibrate(1000000);
    printf("METER calibration_iterations=1000000 calibration_ticks=%u\n", calibration);
    for (unsigned int i = 0; i < count; i++) {
        const struct block *b = &blocks[i];
        for (unsigned int sample = 0; sample < 48; sample++) {
            seed(&base, b->pc, sample);
            candidate = base;
            int remaining = sh2_execute_interpreter(&base, b->cycles);
            if (!b->run(&candidate, b->cycles) || remaining != 0)
                return 2;
            if (memcmp(&base, &candidate, sizeof(base)))
                return fail_state("accepted", i, sample, &base, &candidate);
            comparisons++;
        }
        seed(&before, b->pc, 99);
        for (int budget = -1; budget < b->cycles; budget++) {
            candidate = before;
            if (b->run(&candidate, budget) || memcmp(&before, &candidate, sizeof(before)))
                return fail_state("declined_budget", i, budget, &before, &candidate);
            rejects++;
        }
        for (int reason = 0; reason < 3; reason++) {
            candidate = before;
            if (reason == 0) candidate.delay = b->pc;
            if (reason == 1) candidate.test_irq = 1;
            if (reason == 2) candidate.pc += 2;
            base = candidate;
            if (b->run(&candidate, b->cycles) || memcmp(&base, &candidate, sizeof(base)))
                return fail_state("declined_context", i, reason, &base, &candidate);
            rejects++;
        }
    }
    printf("PROOF blocks=%u state_comparisons=%u unchanged_declines=%u context_bytes=%u\n",
           count, comparisons, rejects, (unsigned)sizeof(SH2));
    unsigned int measured_lengths = 0, measured_roms = 0;
    for (unsigned int i = 0; i < count; i++) {
        const struct block *b = &blocks[i];
        int choose = 0;
        if (strstr(b->name, "synthetic_mixed")) {
            unsigned bit = b->cycles == 4 ? 1 : b->cycles == 8 ? 2 : b->cycles == 16 ? 4 : 8;
            choose = !(measured_lengths & bit);
            measured_lengths |= bit;
        } else if (strstr(b->name, "runtime_observed_alu")) {
            choose = 1;
        } else if (strstr(b->name, "rom_static")) {
            /* ROM-derived samples are semantic examples, not verified hot code. */
            choose = measured_roms++ % 16 == 0;
        }
        if (!choose) continue;
        seed(&before, b->pc, 100);
        base = before; candidate = before;
        const int repeats = 4096;
        uint32_t t0 = rig_timer_now();
        for (int j = 0; j < repeats; j++) {
            base.pc = b->pc;
            sh2_execute_interpreter(&base, b->cycles);
        }
        uint32_t ta = rig_timer_now()-t0;
        t0 = rig_timer_now();
        for (int j = 0; j < repeats; j++) {
            candidate.pc = b->pc;
            if (!b->run(&candidate, b->cycles)) return 3;
        }
        uint32_t tb = rig_timer_now()-t0;
        if (memcmp(&base, &candidate, sizeof(base)))
            return fail_state("bench", i, 0, &base, &candidate);
        printf("BENCH block=%u guest_ops=%d repeats=%d baseline_ticks=%u native_ticks=%u\n",
               i, b->cycles, repeats, ta, tb);
    }
    puts("PASS register-only isolated proof; no memory/branch/IRQ execution, game coverage or device FPS claim");
    return 0;
}
