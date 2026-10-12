/* Bounded native chains: parameter loads, computed jump, coordinate, loop tail. */
#include <string.h>
#include <stdlib.h>
static unsigned chain_calls,chain_parts,chain_proofs,inactive_calls,inactive_iterations;
static unsigned inactive_native(SH2 *s,unsigned pc,unsigned first_callback);
static int chain_proof_active;
static unsigned chain_entry(SH2 *s,unsigned pc){
  if(s->delay || s->test_irq || s->icount<5 || !s->p_sdram || !control_ram(pc) || (pc&1) || (pc&0x3ffffu)>0x3fff8u)return 0;
  const unsigned short *p=(const unsigned short *)((const unsigned char *)s->p_sdram+(pc&0x3ffffu));
  if(p[0]!=0x50e6 || p[1]!=0x51e3 || p[2]!=0x402b || p[3]!=0x58e2)return 0;
  unsigned a=s->r[14];
  if(!control_memory(s,a+24,4) || !control_memory(s,a+12,4) || !control_memory(s,a+8,4))return 0;
  unsigned callback=control_rd32(s,a+24);
#ifdef NATIVE_INACTIVE_FOLD
  unsigned folded=inactive_native(s,pc,callback);if(folded)return folded;
#endif
  s->r[0]=callback;s->r[1]=control_rd32(s,a+12);s->r[8]=control_rd32(s,a+8);
  s->pc=s->r[0];s->ppc=pc+6;s->ea=a+8;s->icount-=5;return 5;
}
static unsigned inactive_native(SH2 *s,unsigned pc,unsigned first_callback){
  if(s->icount<17)return 0;
  static const unsigned short idle[]={0xe100,0xe000,0x201f,0x4129,0x021a,0x201f,0x3c2c,0x021a,0x3b2c,0x4710,0x8fd7,0x7e20};
  unsigned n=0,used=0,a=s->r[14],counter=s->r[7],callback=0,last_a=0;
  while(n<48){
    unsigned cost=counter==1?17:18;
    if(s->icount<(int)(used+cost) || !control_memory(s,a+24,4) || !control_memory(s,a+12,4) || !control_memory(s,a+8,4))break;
    unsigned cb=n?control_rd32(s,a+24):first_callback;
    if(!control_ram(cb) || (cb&1) || (cb&0x3ffffu)>0x3ffe8u || cb-58!=pc)break;
    const unsigned short *q=(const unsigned short *)((const unsigned char *)s->p_sdram+(cb&0x3ffffu));
    if(cb!=callback){unsigned j;for(j=0;j<12;j++)if(q[j]!=idle[j])break;if(j!=12)break;}
    callback=cb;last_a=a;a+=32;counter--;used+=cost;n++;
    if(counter==0)break;
  }
  if(!n)return 0;
  s->r[0]=s->r[1]=s->r[2]=s->macl=0;s->r[8]=control_rd32(s,last_a+8);
  s->r[7]=counter;s->r[14]=a;s->t_flag=counter==0;s->no_polling=SH2_NO_POLLING;
  s->pc=counter?pc:callback+24;s->ppc=callback+22;s->ea=counter?pc:last_a+8;s->icount-=used;
  inactive_calls++;inactive_iterations+=n;return used;
}
static unsigned chain_native(SH2 *s,unsigned pc){
  if(chain_proof_active)return 0;
#ifdef CHAIN_SHADOW_PROOF
  SH2 before,reference;
  if(chain_proofs<2048)before=*s;
#endif
  unsigned used=chain_entry(s,pc),parts=1;
  if(!used)return 0;
  while(parts<48 && s->icount>=5 && !s->delay && !s->test_irq && s->p_sdram && control_ram(s->pc) && !(s->pc&1)){
    unsigned oldpc=s->pc,oldppc=s->ppc;
    unsigned short op=*(unsigned short *)((unsigned char *)s->p_sdram+(oldpc&0x3ffffu));
    if(op!=0x50e6 && op!=0x3d88 && op!=0x201f && op!=0x52e1)break;
    s->ppc=oldpc;s->pc=oldpc+2;
    unsigned c;
    if(op==0x50e6)c=chain_entry(s,oldpc);
    else c=control_native(s,oldpc,op==0x3d88?1:op==0x52e1?2:0);
    if(!c){s->pc=oldpc;s->ppc=oldppc;break;}
    used+=c;parts++;
  }
#ifdef CHAIN_SHADOW_PROOF
  if(chain_proofs<2048){
    reference=before;reference.pc=pc;
    chain_proof_active=1;control_proof_active=1;
    sh2_execute_interpreter(&reference,used);
    chain_proof_active=0;control_proof_active=0;
    reference.icount=before.icount-used;
    if(memcmp(s,&reference,sizeof(*s))){
      printf("CHAIN_FAIL pc=%08x cycles=%u parts=%u\n",pc,used,parts);
      const unsigned *a=(const unsigned *)s,*b=(const unsigned *)&reference;
      for(unsigned i=0;i<sizeof(*s)/4;i++)if(a[i]!=b[i])printf("DIFF word=%u actual=%08x reference=%08x\n",i,a[i],b[i]);
      exit(2);
    }
    chain_proofs++;
  }
#endif
  chain_calls++;chain_parts+=parts;return used;
}
#ifdef CHAIN_SHADOW_PROOF
void chain_synthetic_proof(SH2 *base){
  static unsigned short ram[0x20000];
  static const unsigned short entry[]={0x50e6,0x51e3,0x402b,0x58e2};
  static const unsigned short idle[]={0xe100,0xe000,0x201f,0x4129,0x021a,0x201f,0x3c2c,0x021a,0x3b2c,0x4710,0x8fd7,0x7e20};
  static const unsigned counters[]={0,1,2,16,0xffffffffu};
  unsigned checks=0,declines=0,seed=0x32f01;
  memcpy(ram+0x100,entry,sizeof(entry));memcpy(ram+0x11d,idle,sizeof(idle));
  for(unsigned i=0;i<64;i++){
    unsigned *p=(unsigned *)(ram+0x800+16*i);
    p[2]=i*0x9876543u;p[3]=~i;p[6]=0x023a0600u;
  }
  for(unsigned i=0;i<1024;i++){
    SH2 a=*base,b,original;unsigned pc=0x06000200u;
    a.p_sdram=ram;a.pc=pc+2;a.ppc=pc;a.delay=a.test_irq=0;a.icount=i%513;
    for(unsigned j=0;j<16;j++){seed=seed*1664525u+1013904223u;a.r[j]=seed;}
    a.r[14]=0x06001000u;a.r[7]=counters[i%5];a.t_flag=i&1;
    switch(i%32){case 1:a.delay=pc;break;case 2:a.test_irq=1;break;case 3:a.p_sdram=0;break;case 4:a.r[14]=0x20004000u;break;case 5:pc|=1;break;}
    original=a;b=a;
    unsigned used=chain_native(&a,pc);
    if(!used){if(memcmp(&a,&original,sizeof(a))){printf("CHAIN_DECLINE_FAIL case=%u\n",i);exit(2);}declines++;continue;}
    b.pc=pc;chain_proof_active=1;control_proof_active=1;sh2_execute_interpreter(&b,used);chain_proof_active=0;control_proof_active=0;b.icount=original.icount-used;
    if(memcmp(&a,&b,sizeof(a))){printf("CHAIN_SYNTH_FAIL case=%u budget=%d cycles=%u\n",i,original.icount,used);exit(2);}
    checks++;
  }
  printf("CHAIN_SYNTH checks=%u declines=%u\n",checks,declines);
  chain_calls=chain_parts=chain_proofs=inactive_calls=inactive_iterations=0;
}
#endif
