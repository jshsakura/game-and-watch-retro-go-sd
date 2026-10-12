#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cpu/sh2/sh2.h>
#include "countdown_fold.h"
extern void proof_bind_rom(const uint16_t *,unsigned);
static uint16_t ram[32768];
static SH2 a,b,seed;
int main(void) {
  for(unsigned i=0;i<32768;i++)ram[i]=9;
  proof_bind_rom(ram,sizeof(ram));
  const int budgets[]={1,2,3,4,5,6,7,8,9,13,14,15,16,20,21,22,28,31,63,64,127,511,1024};
  const uint32_t counts[]={1,2,3,4,8,100,1500000,0xffffffffu};
  const uint8_t values[]={0,1,2,3,128,255};
  unsigned comparisons=0,folds=0,rejects=0;
  for(unsigned base=1;base<=3;base++)for(unsigned counter=4;counter<=6;counter++) {
    ram[256]=0x8400|(base<<4)|1;ram[257]=0x2008;ram[258]=0x8b21;
    ram[259]=0x4010|(counter<<8);ram[260]=0x8bfa;
    for(unsigned vi=0;vi<6;vi++)for(unsigned ni=0;ni<8;ni++)for(unsigned bi=0;bi<23;bi++)for(unsigned t=0;t<2;t++) {
      memset(&seed,0,sizeof(seed));seed.p_sdram=ram;seed.pc=0x06000200;seed.ppc=0xfeedfeed;
      seed.ea=0xfafafafa;seed.sr=0x20;seed.t_flag=t;seed.r[base]=0x06001000;
      seed.r[counter]=counts[ni];seed.r[0]=0x12345678;seed.no_polling=1;
      ((uint8_t *)ram)[0x1000]=values[vi]; /* guest byte1001, swapped storage */
      a=seed;b=seed;
      sh2_execute_interpreter(&a,budgets[bi]);
      b.icount=budgets[bi];if(countdown_fold(&b))folds++;
      sh2_execute_interpreter(&b,b.icount);
      if(memcmp(&a,&b,sizeof(a))) {
        unsigned char *x=(void *)&a,*y=(void *)&b;
        for(unsigned j=0;j<sizeof(a);j++)if(x[j]!=y[j]){printf("FAIL base=%u counter=%u value=%u count=%lu budget=%d t=%u offset=%u %02x/%02x\n",base,counter,values[vi],(unsigned long)counts[ni],budgets[bi],t,j,x[j],y[j]);break;}
        return 1;
      }
      comparisons++;
    }
  }
  ram[256]=0x8411;ram[259]=0x4210;
  memset(&seed,0,sizeof(seed));seed.p_sdram=ram;seed.pc=0x06000200;seed.r[1]=0x06001000;
  seed.r[2]=100;seed.icount=511;((uint8_t *)ram)[0x1000]=0;
  for(unsigned reason=0;reason<8;reason++) {
    a=seed;
    if(reason==0)a.delay=a.pc;
    if(reason==1)a.test_irq=1;
    if(reason==2)a.r[1]=0x20004000;
    if(reason==3)a.r[2]=0;
    if(reason==4)a.r[2]=1;
    if(reason==5){ram[256]=0x8401;a.r[0]=0x06001000;}
    if(reason==6){ram[256]=0x8411;ram[259]=0x4110;}
    if(reason==7){ram[259]=0x4210;ram[260]=9;}
    b=a;if(countdown_fold(&b)||memcmp(&a,&b,sizeof(a))){printf("FAIL decline=%u\n",reason);return 2;}rejects++;
  }
  printf("PASS countdown comparisons=%u folded_cases=%u unchanged_declines=%u context_bytes=%u\n",comparisons,folds,rejects,(unsigned)sizeof(SH2));
  return 0;
}
