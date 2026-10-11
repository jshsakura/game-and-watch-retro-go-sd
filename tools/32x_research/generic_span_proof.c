/* Differential span/slice proof against the actual baseline interpreter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cpu/sh2/sh2.h>

struct gnw_fetch_win { unsigned region,mask; unsigned char *base; };
struct gnw_fetch_win gnw_fw_rom;
unsigned gnw_sh2_rom_fetch_mask;
int sh2_reference(SH2 *,int);
int sh2_candidate(SH2 *,int);
unsigned test_span(SH2 *,unsigned);
int test_op(SH2 *,unsigned);
static uint16_t rom[0x20000], ram[0x20000];
static unsigned seed=0x32ace, checks, accepted, declines;
static unsigned rng(void){seed=seed*1664525u+1013904223u;return seed;}
static void fail(const char *what,unsigned op,const SH2 *a,const SH2 *b){
  fprintf(stderr,"FAIL %s case=%u opcode=%04x\n",what,checks,op);
  const unsigned char *x=(const unsigned char *)a,*y=(const unsigned char *)b;
  for(unsigned i=0;i<sizeof *a;i++)if(x[i]!=y[i]){fprintf(stderr,"byte %u: %02x/%02x\n",i,x[i],y[i]);break;}
  exit(2);
}
static void equal(const char *what,unsigned op,const SH2 *a,const SH2 *b){
  checks++; if(memcmp(a,b,sizeof *a))fail(what,op,a,b);
}
static SH2 context(void){
  SH2 s; memset(&s,0,sizeof s);
  for(unsigned i=0;i<16;i++)s.r[i]=rng();
  s.macl=rng();s.mach=rng();s.sr=rng()&0x3f2;s.t_flag=rng()&1;
  s.ea=rng();s.pr=rng();s.gbr=rng();s.vbr=rng();s.ppc=rng();
  s.no_polling=(rng()&1)?SH2_NO_POLLING:0;s.is_slave=rng()&1;
  s.p_sdram=ram;s.p_rom=rom;return s;
}
static void unexpected(const char *name){fprintf(stderr,"Unexpected %s\n",name);exit(3);}
u32 p32x_sh2_read8(u32 a,SH2 *s){(void)a;(void)s;unexpected("read8");return 0;}
u32 p32x_sh2_read16(u32 a,SH2 *s){(void)a;(void)s;unexpected("read16");return 0;}
u32 p32x_sh2_read32(u32 a,SH2 *s){(void)a;(void)s;unexpected("read32");return 0;}
void p32x_sh2_write8(u32 a,u32 d,SH2 *s){(void)a;(void)d;(void)s;unexpected("write8");}
void p32x_sh2_write16(u32 a,u32 d,SH2 *s){(void)a;(void)d;(void)s;unexpected("write16");}
void p32x_sh2_write32(u32 a,u32 d,SH2 *s){(void)a;(void)d;(void)s;unexpected("write32");}
void sh2_do_irq(SH2 *s,int l,int v){(void)s;(void)l;(void)v;unexpected("irq");}
void lprintf(const char *f,...){(void)f;unexpected("illegal/log");}

int main(void){
  static uint16_t allowed[65536];unsigned nallowed=0;
  gnw_fw_rom=(struct gnw_fetch_win){0x02000000,sizeof rom-1,(unsigned char *)rom};
  gnw_sh2_rom_fetch_mask=sizeof rom-2;
  /* Every accepted encoding including every register alias, both T states
   * and unsigned/signed arithmetic edges, against reference dispatch. */
  for(unsigned op=0;op<65536;op++){
    SH2 probe=context(), original=probe;
    int ok=test_op(&probe,op);
    if(!ok){equal("unsupported-op",op,&probe,&original);continue;}
    allowed[nallowed++]=op;accepted++;
    for(unsigned edge=0;edge<4;edge++){
      SH2 a=context(),b;
      if(edge<3)for(unsigned i=0;i<16;i++)a.r[i]=edge==0?0:edge==1?0xffffffffu:(i&1?0x80000000u:0x7fffffffu);
      a.t_flag=edge&1;a.pc=0x02000200;a.ppc=a.pc;rom[0x100]=op;b=a;
      sh2_reference(&a,1);sh2_candidate(&b,1);equal("single-dispatch",op,&a,&b);
    }
  }
  /* Mixed runtime-generated spans, arbitrary register dependencies, exact
   * exhausted/partial budgets and both ordinary/cache-through mappings. */
  for(unsigned j=0;j<24000;j++){
    gnw_fw_rom.mask=sizeof rom-(j&1?1:2);
    unsigned len=1+rng()%64, mode=j%4,pc=(mode&1?0x06000000u:0x02000000u)+0x200;
    if(mode&2)pc|=0x20000000u;
    uint16_t *mem=mode&1?ram:rom;
    for(unsigned i=0;i<len;i++)mem[0x100+i]=allowed[rng()%nallowed];
    mem[0x100]=j&1?0x7000|(rng()&0xfff):0xe000|(rng()&0xfff);
    /* Branch is an unsupported terminator; stop before it in this proof. */
    mem[0x100+len]=0xa000;
    SH2 a=context(),b;a.pc=pc;a.ppc=pc;b=a;
    int budget=j%6==0?0:j%6==1?-1:1+rng()%len;
    if(j%7==0)a.test_irq=b.test_irq=1; /* masked IRQ test must fall back */
    if(j%11==0){a.delay=b.delay=pc;a.pc=b.pc=pc+2;}
    sh2_reference(&a,budget);sh2_candidate(&b,budget);
    if(memcmp(&a,&b,sizeof a)){
      fprintf(stderr,"j=%u mode=%u len=%u budget=%d pc=%08x/%08x ppc=%08x/%08x count=%d/%d\n",j,mode,len,budget,a.pc,b.pc,a.ppc,b.ppc,a.icount,b.icount);
      for(unsigned i=0;i<len;i++)fprintf(stderr,"%04x%c",mem[0x100+i],i+1==len?'\n':' ');
    }
    equal("mixed-slice",mem[0x100],&a,&b);
  }
  /* Direct helper: rejection unchanged and unsupported-next stop exact. */
  for(unsigned j=0;j<1024;j++){
    SH2 a=context(),b,original;unsigned pc=0x02000200;
    a.ppc=pc;a.pc=pc+2;a.icount=4;rom[0x100]=0xe07f;rom[0x101]=0xa000;
    switch(j%8){case 0:a.delay=pc;break;case 1:a.test_irq=1;break;case 2:a.icount=0;break;
      case 3:a.icount=-1;break;case 4:a.ppc|=1;break;case 5:a.ppc=0xc0000200;break;}
    original=a;b=a;unsigned done=test_span(&a,rom[0x100]);
    if(!done){declines++;equal("decline",rom[0x100],&a,&original);}
    else{b.pc=pc;sh2_reference(&b,done);b.icount=original.icount-done;equal("terminator",rom[0x100],&a,&b);}
  }
  /* Mapping wrap: consume final word, leave following fetch to reference. */
  for(unsigned region=0;region<4;region++){
    unsigned pc=(region&1?0x06000000u:0x02000000u)+sizeof rom-2;
    if(region&2)pc|=0x20000000u;
    uint16_t *mem=region&1?ram:rom;mem[0x1ffff]=0x707f;
    SH2 a=context(),b,original;a.ppc=pc;a.pc=pc+2;a.icount=20;original=a;b=a;
    unsigned done=test_span(&a,0x707f);if(done!=1)unexpected("mapping-overrun");
    b.pc=pc;sh2_reference(&b,1);b.icount=original.icount-1;equal("mapping-end",0x707f,&a,&b);
  }
  printf("PASS generic span checks=%u accepted_encodings=%u declines=%u context_bytes=%u\n",checks,accepted,declines,(unsigned)sizeof(SH2));
  return 0;
}
