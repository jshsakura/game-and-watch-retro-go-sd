/* Full-context differential proof for automatically generated read/ALU blocks. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "native_module.h"
struct gnw_fetch_win { unsigned region,mask; unsigned char *base; };
struct gnw_fetch_win gnw_fw_rom;
unsigned gnw_sh2_rom_fetch_mask;
unsigned long long g_sh2_insns;
extern const struct aot_entry aot_entries[];
extern int sh2_aot_reference(SH2*,int);
static uint16_t *rom,*ram;
static unsigned seed=0xa0732,checks,declines,full,partial;
static unsigned rng(void){seed=seed*1664525u+1013904223u;return seed;}
static void bad(const char *s){printf("FAIL unexpected %s\n",s);exit(3);}
static unsigned readmem(unsigned a,unsigned w,SH2 *s){
  unsigned h=a&0xdf000000u,off;unsigned char *p;
  if(h==0x06000000u){p=s->p_sdram;off=a&0x3ffffu;}
  else if(h==0x02000000u){p=s->p_rom;off=a&gnw_sh2_rom_fetch_mask;}
  else{bad("memory callback");return 0;}
  if(w==1)return p[off^1];
  if(w==2)return *(uint16_t *)(p+off);
  unsigned v=*(uint32_t *)(p+off);return (v<<16)|(v>>16);
}
u32 p32x_sh2_read8(u32 a,SH2*s){return readmem(a,1,s);}
u32 p32x_sh2_read16(u32 a,SH2*s){return readmem(a,2,s);}
u32 p32x_sh2_read32(u32 a,SH2*s){return readmem(a,4,s);}
void p32x_sh2_write8(u32 a,u32 d,SH2*s){(void)a;(void)d;(void)s;bad("write8");}
void p32x_sh2_write16(u32 a,u32 d,SH2*s){(void)a;(void)d;(void)s;bad("write16");}
void p32x_sh2_write32(u32 a,u32 d,SH2*s){(void)a;(void)d;(void)s;bad("write32");}
void sh2_do_irq(SH2*s,int l,int v){(void)s;(void)l;(void)v;bad("IRQ");}
void lprintf(const char*f,...){(void)f;bad("illegal");}
static void equal(SH2*a,SH2*b,unsigned pc,unsigned sample,unsigned used){
  checks++;
  if(memcmp(a,b,sizeof *a)){
    printf("FAIL context pc=%08x sample=%u used=%u\n",pc,sample,used);
    const unsigned *x=(const unsigned *)a,*y=(const unsigned *)b;
    for(unsigned j=0;j<sizeof *a/4;j++)if(x[j]!=y[j]){printf("word=%u ref=%08x actual=%08x\n",j,x[j],y[j]);break;}
    exit(4);
  }
}
static SH2 context(unsigned pc,unsigned mode){
  SH2 s;memset(&s,0,sizeof s);
  for(unsigned j=0;j<16;j++){
    switch(mode%5){
      case 0:s.r[j]=0x06010000+j*16;break;
      case 1:s.r[j]=0x02010000+j*16;break;
      case 2:s.r[j]=rng();break;
      case 3:s.r[j]=(j&1)?0xffffffffu:0x80000000u;break;
      default:s.r[j]=j*8;break;
    }
  }
  s.gbr=mode&1?0x26010000:0x02010000;s.sr=rng()&0x3f2;s.t_flag=mode&1;
  s.no_polling=(mode&2)?SH2_NO_POLLING:0;s.mach=rng();s.macl=rng();s.pr=rng();s.vbr=rng();s.ea=rng();
  s.is_slave=mode&1;s.p_sdram=ram;s.p_rom=rom;s.pc=pc+2;s.ppc=pc;s.icount=80;return s;
}
int main(void){
  rom=malloc(4*1024*1024);ram=malloc(256*1024);if(!rom||!ram)bad("allocation");
  for(unsigned j=0;j<4*1024*1024/2;j++)rom[j]=rng();
  for(unsigned j=0;j<256*1024/2;j++)ram[j]=rng();
  gnw_fw_rom=(struct gnw_fetch_win){0x02000000,4*1024*1024-1,(unsigned char *)rom};
  gnw_sh2_rom_fetch_mask=4*1024*1024-1;
  for(unsigned i=0;i<AOT_BLOCK_COUNT;i++){
    const struct aot_entry *e=&aot_entries[i];
    uint16_t *live=((e->pc&0xdf000000u)==0x06000000u)?ram+(e->pc&0x3fffe)/2:rom+(e->pc&0x3ffffe)/2;
    memcpy(live,e->ops,e->n*2);
    for(unsigned mode=0;mode<96;mode++){
      SH2 a=context(e->pc,mode),b=a,original=a;
      a.pc=e->pc;unsigned used=aot_enter(&b,e);
      if(used){unsigned spent=original.icount-b.icount;sh2_aot_reference(&a,spent);a.icount=original.icount-spent;
        if(used==e->n)full++;else partial++;
      }else{a=original;declines++;}
      equal(&a,&b,e->pc,mode,used);
    }
    for(unsigned reason=0;reason<10;reason++){
      SH2 a=context(e->pc,0),b;
      if(reason==0)a.delay=e->pc;
      if(reason==1)a.test_irq=1;
      if(reason==2)a.icount=e->cycles-1;
      if(reason==3)a.icount=0;
      if(reason==4)a.ppc|=1;
      if(reason==5){if((e->pc&0xdf000000u)==0x06000000u)a.p_sdram=0;else gnw_fw_rom.base=0;}
      if(reason==6 || reason==7)live[reason==6?0:e->n-1]^=1;
      if(reason>=8 && (e->pc&0xdf000000u)!=0x02000000u)continue;
      if(reason==8)gnw_fw_rom.region=0xffffffffu;
      if(reason==9)gnw_fw_rom.mask=0;
      b=a;unsigned used=aot_enter(&b,e);if(used)bad("guard accepted");
      equal(&a,&b,e->pc,reason+96,0);declines++;
      gnw_fw_rom.base=(unsigned char *)rom;
      gnw_fw_rom.region=0x02000000u;gnw_fw_rom.mask=4*1024*1024-1;
      if(reason==6 || reason==7)live[reason==6?0:e->n-1]^=1;
    }
  }
  printf("PASS aot blocks=%u checks=%u full=%u partial=%u declines=%u context_bytes=%u\n",AOT_BLOCK_COUNT,checks,full,partial,declines,(unsigned)sizeof(SH2));
  return 0;
}
