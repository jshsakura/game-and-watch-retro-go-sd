/* Compare the real packed picker with the pre-change scalar algorithm.
 * Concurrent ISR cursor movement is deliberately excluded; hardware verifies
 * the coherent-window change separately. Includes wrap and full-scale inputs. */
#include <stdio.h>
#include "../../Core/Src/porting/snes/snes_audio_stretch.c"
#ifdef __arm__
extern void rig_timer_init(void);
extern uint32_t rig_timer_now(void);
#endif
static uint16_t reference(uint16_t *conf) {
  int64_t best=INT64_MIN, energy=0; uint16_t period=REPEAT;
  for (unsigned k=0;k<128;k+=2) { int32_t a=ring[(rd-1u-k)&RING_MASK]; energy+=(int64_t)a*a; }
  for (unsigned lag=LOOP_MIN;lag<=LOOP_MAX;lag+=2) {
    int64_t acc=0;
    for (unsigned k=0;k<128;k+=2) acc+=(int64_t)ring[(rd-1u-k)&RING_MASK]*ring[(rd-1u-k-lag)&RING_MASK];
    if (acc>best) { best=acc;period=lag; }
  }
  *conf=0;
  if (energy>0 && best>0) { int64_t q=(best<<8)/energy; *conf=q>255?255:q; }
  return period;
}
int main(void) {
  unsigned cases=0; uint32_t seed=7;
#ifdef __arm__
  rig_timer_init();
#endif
  for(unsigned pattern=0;pattern<5;pattern++) {
    for(unsigned i=0;i<RING;i++) {
      seed=seed*1664525u+1013904223u;
      ring[i]=pattern==0?0:pattern==1?-32768:pattern==2?(i&1?32767:-32768):pattern==3?(int16_t)(i*137u):(int16_t)(seed>>16);
    }
    for(unsigned cursor=0;cursor<RING;cursor+=17) {
      uint16_t conf; rd=cursor; uint16_t want=reference(&conf), got=stretch_pick_period();
      if(want!=got || conf!=pitch_conf) { printf("FAIL cursor=%u pattern=%u lag=%u/%u conf=%u/%u\n",cursor,pattern,want,got,conf,pitch_conf); return 1; }
      cases++;
    }
  }
  printf("PASS %u picker windows (wrap, silence, extremes, ramp, noise)\n",cases);
#ifdef __arm__
  rd=123; uint16_t conf; uint32_t a=rig_timer_now();
  for(unsigned i=0;i<16;i++) { volatile uint16_t p=reference(&conf); (void)p; }
  uint32_t scalar=rig_timer_now()-a; a=rig_timer_now();
  for(unsigned i=0;i<16;i++) { volatile uint16_t p=stretch_pick_period(); (void)p; }
  printf("QEMU instruction timer scalar=%lu packed=%lu (not hardware cycles)\n",(unsigned long)scalar,(unsigned long)(rig_timer_now()-a));
#endif
  return 0;
}
