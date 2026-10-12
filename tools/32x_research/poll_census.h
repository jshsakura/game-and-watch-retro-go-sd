/* Diagnostic only: counts the host iterations left inside existing folds.
 * The guest's reads, registers, cycles and scheduler remain unchanged. */
#include <stdio.h>
#include <string.h>

extern int rig_frame_no;

struct poll_census_total {
    unsigned long long calls, active, iterations, removable_reads;
    unsigned long long value_changes, indirect_calls, max_iterations;
};
static struct poll_census_total poll_census[2][3];

struct poll_census_sample {
    struct poll_census_total *total;
    unsigned iterations, previous, changes, direct;
};

static struct poll_census_sample poll_census_begin(SH2 *s, unsigned kind, unsigned a)
{
    struct poll_census_sample sample = {0};
    if (rig_frame_no < RIG_WARMUP || rig_frame_no >= RIG_FRAMES)
        return sample;
    sample.total = &poll_census[s->is_slave & 1][kind];
    sample.total->calls++;
    /* RW tests the ROM window before SDRAM; RL tests SDRAM first.
     * Only count the actual direct SDRAM route as an elimination candidate. */
    sample.direct = ((a & 0xdf000000u) == 0x06000000u) && s->p_sdram &&
                    (kind == 1 || gnw_fw_rom.region != 0x06000000u);
    if (!sample.direct)
        sample.total->indirect_calls++;
    return sample;
}

static void poll_census_read(struct poll_census_sample *sample, unsigned value)
{
    if (!sample->total)
        return;
    if (sample->iterations && value != sample->previous)
        sample->changes++;
    sample->previous = value;
    sample->iterations++;
}

static void poll_census_end(struct poll_census_sample *sample, unsigned kind)
{
    struct poll_census_total *total = sample->total;
    if (!total)
        return;
    unsigned n = sample->iterations;
    total->active += n != 0;
    total->iterations += n;
    total->value_changes += sample->changes;
    if (n > total->max_iterations)
        total->max_iterations = n;
    /* Conservatively retain two initial iterations for BT/S, one otherwise.
     * This is an observed opportunity, not a proof that a fold is correct. */
    unsigned keep = kind == 1 ? 2 : 1;
    if (sample->direct && !sample->changes && n > keep)
        total->removable_reads += n - keep;
}

void poll_census_report(void)
{
    for (unsigned core = 0; core < 2; core++)
        for (unsigned kind = 0; kind < 3; kind++) {
            struct poll_census_total *t = &poll_census[core][kind];
            printf("POLL_CENSUS %u %u %llu %llu %llu %llu %llu %llu %llu\n",
                   core, kind, t->calls, t->active, t->iterations,
                   t->removable_reads, t->value_changes, t->indirect_calls,
                   t->max_iterations);
        }
    printf("POLL_CENSUS_END\n");
}
