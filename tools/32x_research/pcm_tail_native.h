/* Pure arithmetic/sample limiting tail; leaves ring-buffer/MMIO writes alone. */
#include <string.h>
#include <stdlib.h>
static unsigned pcm_tail_calls,pcm_tail_proofs;
static int pcm_tail_proof_active;
static unsigned pcm_tail_word(SH2 *s,unsigned a){
  const unsigned char *base;unsigned off;
  if(control_ram(a)){base=s->p_sdram;off=a&0x3fffeu;}
  else{base=gnw_fw_rom.base;off=a&gnw_fw_rom.mask;}
  return (unsigned)(int)(short)*(const unsigned short *)(base+off);
}
static unsigned pcm_tail_native(SH2 *s,unsigned pc){
  if(pcm_tail_proof_active || s->delay || s->test_irq || s->icount<25 || !s->p_sdram || !control_ram(pc) || (pc&1) || (pc&0x3ffffu)>0x3ffd6u)return 0;
  static const unsigned short ops[]={0xd21c,0x951c,0x3b2d,0x911e,0x0b0a,0x3b5c,0x3c2d,0x4b15,0x8901,0x31b3,0xeb01,0x8900,0x6b13,0x0c0a,0x3c5c,0x4c15,0x8901,0x31c3,0xec01,0x8900,0x6c13};
  const unsigned short *p=(const unsigned short *)((const unsigned char *)s->p_sdram+(pc&0x3ffffu));
  for(unsigned i=0;i<21;i++)if(p[i]!=ops[i])return 0;
  unsigned a2=((pc+4)&~3u)+0x70,a5=pc+0x3e,a1=pc+0x46;
  if(!control_memory(s,a2,4) || !control_memory(s,a5,2) || !control_memory(s,a1,2))return 0;
  unsigned r2=control_rd32(s,a2),r5=pcm_tail_word(s,a5),r1=pcm_tail_word(s,a1),ea=a1,cycles=10;
  unsigned long long left=(unsigned long long)((long long)(int)s->r[11]*(int)r2),right=(unsigned long long)((long long)(int)s->r[12]*(int)r2);
  unsigned r11=(unsigned)(left>>32)+r5,r12=(unsigned)(right>>32)+r5,t=(int)r11>0;
  if(t){cycles+=3;ea=pc+22;}else{cycles+=3;t=(int)r1>=(int)r11;r11=1;}
  if(t){cycles+=3;ea=pc+26;}else{cycles+=2;r11=r1;}
  cycles+=3;t=(int)r12>0;
  if(t){cycles+=3;ea=pc+38;}else{cycles+=3;t=(int)r1>=(int)r12;r12=1;}
  unsigned ppc;
  if(t){cycles+=3;ea=pc+42;ppc=pc+38;}else{cycles+=2;r12=r1;ppc=pc+40;}
#ifdef PCM_TAIL_SHADOW_PROOF
  SH2 reference;
  if(pcm_tail_proofs<2048){reference=*s;reference.pc=pc;pcm_tail_proof_active=1;sh2_execute_interpreter(&reference,cycles);pcm_tail_proof_active=0;reference.icount=s->icount-cycles;}
#endif
  s->r[1]=r1;s->r[2]=r2;s->r[5]=r5;s->r[11]=r11;s->r[12]=r12;s->macl=(unsigned)right;s->mach=(unsigned)(right>>32);s->t_flag=t;s->ea=ea;s->pc=pc+42;s->ppc=ppc;s->icount-=cycles;
#ifdef PCM_TAIL_SHADOW_PROOF
  if(pcm_tail_proofs<2048){if(memcmp(s,&reference,sizeof(*s))){printf("PCM_TAIL_FAIL pc=%08x cycles=%u\n",pc,cycles);exit(2);}pcm_tail_proofs++;}
#endif
  pcm_tail_calls++;return cycles;
}
#ifdef PCM_TAIL_SHADOW_PROOF
void pcm_tail_synthetic_proof(SH2 *base){
  static unsigned short ram[0x1000];
  static const unsigned short ops[]={0xd21c,0x951c,0x3b2d,0x911e,0x0b0a,0x3b5c,0x3c2d,0x4b15,0x8901,0x31b3,0xeb01,0x8900,0x6b13,0x0c0a,0x3c5c,0x4c15,0x8901,0x31c3,0xec01,0x8900,0x6c13};
  static const unsigned edge[]={0,1,2,0x7fff,0x8000,0xffff,0x10000,0x7fffffff,0x80000000,0xffffffff};
  unsigned seed=0x32b15,checks=0,declines=0;
  memcpy(ram+0x100,ops,sizeof(ops));
  for(unsigned i=0;i<4096;i++){
    SH2 a=*base,b,original;unsigned pc=0x06000200u;
    a.p_sdram=ram;a.pc=pc+2;a.ppc=pc;a.delay=a.test_irq=0;a.icount=i%32;a.t_flag=i&1;
    seed=seed*1664525u+1013904223u;a.r[11]=i<100?edge[i/10]:seed;
    seed=seed*1664525u+1013904223u;a.r[12]=i<100?edge[i%10]:seed;
    unsigned gain=i<100?edge[(i/10+i%10)%10]:seed;
    *(unsigned *)(ram+0x13a)=(gain<<16)|(gain>>16);
    ram[0x11f]=(unsigned short)(seed>>8);ram[0x123]=(unsigned short)seed;
    unsigned g=i%16;
    if(g>=1 && g<=5)a.icount=64;
    switch(g){case 1:a.delay=pc;break;case 2:a.test_irq=1;break;case 3:a.p_sdram=0;break;case 4:pc|=1;break;case 5:pc=0x02000200u;break;}
    original=a;b=a;unsigned used=pcm_tail_native(&a,pc);
    if(!used){if(memcmp(&a,&original,sizeof(a))){printf("PCM_TAIL_DECLINE_FAIL case=%u\n",i);exit(2);}declines++;continue;}
    b.pc=pc;pcm_tail_proof_active=1;sh2_execute_interpreter(&b,used);pcm_tail_proof_active=0;b.icount=original.icount-used;
    if(memcmp(&a,&b,sizeof(a))){printf("PCM_TAIL_SYNTH_FAIL case=%u cycles=%u\n",i,used);exit(2);}checks++;
  }
  printf("PCM_TAIL_SYNTH checks=%u declines=%u\n",checks,declines);
  pcm_tail_calls=pcm_tail_proofs=0;
}
#endif
