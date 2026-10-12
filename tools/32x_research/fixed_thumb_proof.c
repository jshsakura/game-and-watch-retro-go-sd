/* Raw-opcode fixed-register engine vs the complete existing interpreter. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cpu/sh2/sh2.h>
#include "fixed_fixtures.h"
struct gnw_fetch_win { unsigned region,mask; unsigned char *base; };
struct gnw_fetch_win gnw_fw_rom;
unsigned gnw_sh2_rom_fetch_mask;
extern int sh2_fixed_reference(SH2*,int);
static uint16_t *rom,*ram;
static unsigned seed=0x73f18,checks,irqs;
struct write_event { unsigned addr,data,width,pc,ppc,t,cycles,ea; };
static struct write_event writes[8];static unsigned nwrites,write_effect;
static unsigned rng(void){seed=seed*1664525u+1013904223u;return seed;}
static void bad(const char*s){printf("FAIL %s\n",s);exit(3);}
static unsigned memread(unsigned a,unsigned w,SH2*s){
 unsigned h=a&0xdf000000u;const unsigned char*p;unsigned off;
 if(h==0x06000000u){p=s->p_sdram;off=a&(w==1?0x3ffff:w==2?0x3fffe:0x3fffc);}
 else if(h==0x02000000u){p=s->p_rom;off=a&gnw_sh2_rom_fetch_mask;}
 else{printf("read addr=%08x width=%u pc=%08x ppc=%08x delay=%08x\n",a,w,s->pc,s->ppc,s->delay);bad("unexpected read region");return 0;}
 if(w==1)return p[off^1];if(w==2)return *(const uint16_t*)(p+off);
 unsigned v=*(const uint32_t*)(p+off);return (v<<16)|(v>>16);
}
u32 p32x_sh2_read8(u32 a,SH2*s){return memread(a,1,s);}
u32 p32x_sh2_read16(u32 a,SH2*s){return memread(a,2,s);}
u32 p32x_sh2_read32(u32 a,SH2*s){return memread(a,4,s);}
static void memwrite(u32 a,u32 d,SH2*s,unsigned width){
 if(nwrites>=8)bad("write log overflow");
 writes[nwrites++]=(struct write_event){a,d,width,s->pc,s->ppc,s->t_flag,(unsigned)s->icount,s->ea};
 if(write_effect==1)s->icount-=3;
 if(write_effect>=2){s->test_irq=1;s->pending_level=write_effect==2?15:0;}
}
void p32x_sh2_write8(u32 a,u32 d,SH2*s){memwrite(a,d,s,1);}
void p32x_sh2_write16(u32 a,u32 d,SH2*s){memwrite(a,d,s,2);}
void p32x_sh2_write32(u32 a,u32 d,SH2*s){memwrite(a,d,s,4);}
void sh2_do_irq(SH2*s,int level,int vector){irqs++;s->pc=0x02004000;s->sr=(s->sr&~0xf0)|(level<<4);s->icount-=5;(void)vector;}
void lprintf(const char*f,...){(void)f;bad("illegal opcode");}
static int irqcb(SH2*s,int l){(void)s;return l+64;}
static SH2 context(unsigned pc,unsigned mode,unsigned op){
 SH2 s;memset(&s,0,sizeof s);
 for(unsigned j=0;j<16;j++){
  switch(mode%6){case 0:s.r[j]=rng();break;case 1:s.r[j]=0;break;
   case 2:s.r[j]=0xffffffffu;break;case 3:s.r[j]=(j&1)?0x80000000u:0x7fffffffu;break;
   case 4:s.r[j]=0x06010000+j*16+(mode&3);break;
   default:s.r[j]=0x02010000+j*16+(mode&3);break;}
 }
 unsigned major=op>>12,m=(op>>4)&15;
 int read=major==5 || major==9 || major==13 ||
  (major==6 && ((op&15)<=2 || ((op&15)>=4 && (op&15)<=6))) ||
  (major==0 && (op&15)>=12 && (op&15)<=14) ||
  (major==8 && (op>>8==0x84 || op>>8==0x85));
 if(read && major!=9 && major!=13){s.r[m]=(mode&1?0x26010000:0x02010000)+(mode&3);}
 if(major==0 && (op&15)>=12 && (op&15)<=14){s.r[0]=0x03008000;s.r[m]=0x03008000;}
 s.gbr=(mode&1?0x26010000:0x02010000)+(mode&3);
 s.pc=pc;s.ppc=rng();s.t_flag=mode&1;s.sr=rng()&0x3f3;s.no_polling=(mode&2)?SH2_NO_POLLING:0;
 s.mach=rng();s.macl=rng();s.pr=rng();s.vbr=rng();s.ea=rng();s.icount=(int)rng();
 if(major==10 || major==11){s.vbr=0x02010000;s.r[15]=0x06020000;}
 s.p_sdram=ram;s.p_rom=rom;s.is_slave=mode&1;s.irq_callback=irqcb;
 return s;
}
static void compare(SH2 initial,int cycles,unsigned op,unsigned mode){
 SH2 a=initial,b=initial;struct write_event expected[8];
 write_effect=mode%4;nwrites=0;int ca=sh2_fixed_reference(&a,cycles);unsigned expected_n=nwrites;
 memcpy(expected,writes,sizeof writes);nwrites=0;int cb=sh2_execute_interpreter(&b,cycles);checks++;
 if(ca!=cb || memcmp(&a,&b,sizeof a) || nwrites!=expected_n || memcmp(expected,writes,nwrites*sizeof *writes)){
  printf("FAIL op=%04x mode=%u cycles=%d pc=%08x returns=%d/%d\n",op,mode,cycles,initial.pc,ca,cb);
  const unsigned*x=(const unsigned*)&a,*y=(const unsigned*)&b;
  for(unsigned j=0;j<sizeof a/4;j++)if(x[j]!=y[j]){printf("word=%u ref=%08x actual=%08x\n",j,x[j],y[j]);break;}
  exit(4);
 }
}
int main(void){
 rom=malloc(4*1024*1024+16);ram=malloc(256*1024);if(!rom||!ram)bad("allocation");
 for(unsigned j=0;j<(4*1024*1024+16)/2;j++)rom[j]=rng();
 for(unsigned j=0;j<256*1024/2;j++)ram[j]=rng();
 gnw_fw_rom=(struct gnw_fetch_win){0x02000000,4*1024*1024-1,(unsigned char*)rom};gnw_sh2_rom_fetch_mask=4*1024*1024-1;
 unsigned pc=0x02002000;
 for(unsigned i=0;i<FIXED_OP_COUNT;i++){
  unsigned op=fixed_ops[i];rom[0x2000/2]=op;rom[0x2000/2+1]=9;
  compare(context(pc,i,op),1,op,i);
 }
 for(unsigned i=0;i<FIXED_EDGE_COUNT;i++){
  unsigned op=fixed_edges[i];
  for(unsigned mode=0;mode<24;mode++){
   unsigned addr=(mode&8)?0x26002000:pc;uint16_t*code=(mode&8)?ram+0x2000/2:rom+0x2000/2;code[0]=op;code[1]=9;code[0x100/2]=9;
   SH2 s=context(addr,mode,op);compare(s,1,op,mode);
   compare(s,0,op,mode);compare(s,-1,op,mode);
   s.test_irq=1;s.pending_level=15;compare(s,1,op,mode);
   s.test_irq=0;s.delay=addr;s.pc=addr+0x100;compare(s,1,op,mode);
  }
 }
 /* Interleave supported instructions and conditional/delayed branches. The
    bounded budget exercises continuation through slots after cycles run out. */
 const uint16_t stream[]={0xe101,0x7101,0x3010,0x8900,0xa001,0x7001,0x7102,0x7203,0x4121,0x0009,0x7304,0x0009};
 memcpy(rom+0x2000/2,stream,sizeof stream);
 for(unsigned mode=0;mode<24;mode++)for(int cyc=1;cyc<10;cyc++)compare(context(pc,mode,0x0009),cyc,0xffff,mode);
 /* Disabled direct ROM windows must retain the original bus fallback. */
 rom[0x2000/2]=0x7123;gnw_fw_rom.region=0xffffffffu;
 compare(context(pc,0,0x7123),1,0x7123,99);gnw_fw_rom.region=0x02000000u;
 printf("PASS fixed-thumb checks=%u opcodes=%u edge_fixtures=%u context_bytes=%u irq_calls=%u\n",checks,FIXED_OP_COUNT,FIXED_EDGE_COUNT,(unsigned)sizeof(SH2),irqs);
 return 0;
}
