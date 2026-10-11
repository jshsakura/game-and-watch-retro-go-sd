/* Offline only: observed dispatch runs, not a device timing instrument. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
extern int rig_frame_no;
#define TRACE_SLOTS 8192
#define TRACE_OPS 32
struct trace_run {
  uint32_t pc, last, hash, n, core, delay;
  uint16_t op[TRACE_OPS];
  uint64_t count;
};
static struct trace_run trace_table[TRACE_SLOTS], trace_pending[2];
static uint64_t trace_observed[2], trace_dropped, trace_regions[2][4];
static void trace_flush(unsigned c) {
  struct trace_run *p = &trace_pending[c];
  if (!p->n) return;
  unsigned slot = p->hash & (TRACE_SLOTS-1);
  for (unsigned i=0; i<64; i++, slot=(slot+1)&(TRACE_SLOTS-1)) {
    struct trace_run *e = &trace_table[slot];
    if (!e->count) { *e = *p; e->count=1; p->n=0; return; }
    if (e->hash==p->hash && e->pc==p->pc && e->n==p->n &&
        e->core==c && e->delay==p->delay &&
        !memcmp(e->op,p->op,p->n*sizeof(p->op[0]))) {
      e->count++; p->n=0; return;
    }
  }
  trace_dropped += p->n; p->n=0;
}
static void trace_tick(SH2 *s, uint16_t op, unsigned delay) {
  unsigned c=s->is_slave&1;
  if (rig_frame_no < TRACE_FROM || rig_frame_no >= TRACE_TO) return;
  uint32_t pc=s->ppc, a=pc & ~0x20000000u;
  unsigned region = (a>=0x02000000u && a<0x02400000u) ? 0 :
                    (a>=0x06000000u && a<0x06040000u) ? 1 :
                    (a>=0xc0000000u && a<0xc0001000u) ? 2 : 3;
  trace_observed[c]++; trace_regions[c][region]++;
  struct trace_run *p=&trace_pending[c];
  if (p->n && (pc!=p->last+2 || delay || p->delay || p->n==TRACE_OPS)) trace_flush(c);
  if (!p->n) {
    p->pc=pc; p->core=c; p->delay=delay;
    p->hash=(2166136261u^pc)*16777619u;
    p->hash=(p->hash^c)*16777619u;
    p->hash=(p->hash^delay)*16777619u;
  }
  p->op[p->n++]=op; p->last=pc;
  p->hash=(p->hash^op)*16777619u;
}
void trace_report(void) {
  trace_flush(0); trace_flush(1);
  for (unsigned c=0;c<2;c++)
    printf("TRACE_CORE %u %llu %llu %llu %llu %llu\n", c,
      (unsigned long long)trace_observed[c],
      (unsigned long long)trace_regions[c][0],(unsigned long long)trace_regions[c][1],
      (unsigned long long)trace_regions[c][2],(unsigned long long)trace_regions[c][3]);
  printf("TRACE_DROPPED %llu\n",(unsigned long long)trace_dropped);
  for (unsigned i=0;i<TRACE_SLOTS;i++) {
    struct trace_run *e=&trace_table[i];
    if (!e->count) continue;
    printf("TRACE_RUN %u %08lx %u %llu",e->core,(unsigned long)e->pc,e->delay,
           (unsigned long long)e->count);
    for(unsigned j=0;j<e->n;j++) printf(" %04x",e->op[j]);
    printf("\n");
  }
  printf("TRACE_END\n");
}
