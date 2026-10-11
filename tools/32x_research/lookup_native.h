/* Native repeated SDRAM word lookup / framebuffer span. No fixed PC. */
#include <string.h>
#include <stdlib.h>
static unsigned lookup_calls,lookup_groups,lookup_proofs;
static int lookup_proof_active;
static unsigned lookup_native(SH2 *s,unsigned pc){
  if(lookup_proof_active || s->delay || s->test_irq || s->icount<3 || !s->p_sdram || !s->p_dram || (pc&0xdf000000u)!=0x06000000u || (pc&1))return 0;
  unsigned n=0,r0=s->r[0],r1=s->r[1],r6=s->r[6],r8=s->r[8],t=s->t_flag,ea=s->ea;
  unsigned data[8],ops=0;
  const unsigned short *p=(const unsigned short *)((const unsigned char *)s->p_sdram+(pc&0x3ffffu));
  while(n<8){
    unsigned count=n?4:3;
    if(s->icount<(int)(ops+count) || (pc&0x3ffffu)+2*(ops+count)>0x40000u)break;
    const unsigned short *q=p+ops;
    if(n){if(q[0]!=0x38de)break;q++;}
    if(q[0]!=0x010d || q[1]!=0x30ee || q[2]!=0x2615)break;
    unsigned a=r0+r0,b=r6-2;
    if((a&0xdf000000u)!=0x06000000u || (b&0xdf000000u)!=0x04000000u || (b&1))break;
    unsigned old,tmp;
    if(n){old=r8;tmp=old+s->r[13];r8=tmp+t;t=(old>tmp)||(tmp>r8);}
    r1=(unsigned)(int)(short)*(const unsigned short *)((const unsigned char *)s->p_sdram+(a&0x3fffeu));
    old=r0;tmp=old+s->r[14];r0=tmp+t;t=(old>tmp)||(tmp>r0);
    data[n]=r1&65535;r6=b;ea=a;n++;ops+=count;
  }
  if(!n)return 0;
#ifdef LOOKUP_SHADOW_PROOF
  SH2 reference;unsigned short *slots[8],values[8],expected[8];
  if(lookup_proofs<2048){
    for(unsigned i=0;i<n;i++){slots[i]=(unsigned short *)s->p_dram+(((s->r[6]-2*(i+1))&0x1ffffu)>>1);values[i]=*slots[i];}
    reference=*s;reference.pc=pc;
    lookup_proof_active=1;sh2_execute_interpreter(&reference,ops);lookup_proof_active=0;
    reference.icount=s->icount-ops;
    for(unsigned i=0;i<n;i++)expected[i]=*slots[i];
    for(unsigned i=0;i<n;i++)*slots[i]=values[i];
  }
#endif
  for(unsigned i=0;i<n;i++){
    unsigned a=s->r[6]-2*(i+1),d=data[i];
    unsigned short *pd=(unsigned short *)s->p_dram+((a&0x1ffffu)>>1);
    if(a&0x20000u){unsigned old=*pd;if(!(d&255))d|=old&255;if(!(d&0xff00))d|=old&0xff00;}
    *pd=d;
  }
  s->r[0]=r0;s->r[1]=r1;s->r[6]=r6;s->r[8]=r8;s->t_flag=t;s->ea=ea;s->pc=pc+2*ops;s->ppc=s->pc-2;s->icount-=ops;
#ifdef LOOKUP_SHADOW_PROOF
  if(lookup_proofs<2048){
    unsigned bad=0;for(unsigned i=0;i<n;i++)bad|=*slots[i]!=expected[i];
    if(memcmp(s,&reference,sizeof(*s)) || bad){printf("LOOKUP_FAIL pc=%08x context=%d memory=%u\n",pc,memcmp(s,&reference,sizeof(*s)),bad);exit(2);}
    lookup_proofs++;
  }
#endif
  lookup_calls++;lookup_groups+=n;return ops;
}
