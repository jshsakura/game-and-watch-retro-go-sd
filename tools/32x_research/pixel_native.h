/* Offline generic opcode-pattern native kernel; no title or PC binding. */
static unsigned pixel_calls, pixel_proofs, pixel_declines, pixel_groups;
static int pixel_proof_active;
static void pixel_store(SH2 *s, unsigned a, unsigned d) {
  unsigned short *p=(unsigned short *)s->p_dram+((a&0x1ffffu)>>1);
  if(a&0x20000u){unsigned v=*p;if(!(d&255))d|=v&255;if(!(d&0xff00))d|=v&0xff00;}
  *p=d;
}
/* Called after the first opcode fetch, before it executes. */
static unsigned pixel_native(SH2 *s, unsigned pc) {
  if(pixel_proof_active || s->delay || s->test_irq || s->icount<8 || !s->p_sdram || !s->p_dram)return 0;
  if((pc&0xdf000000u)!=0x06000000u || (pc&0x3ffffu)>0x3fff0u || (pc&1))return 0;
  const unsigned short *p=(const unsigned short *)((const unsigned char *)s->p_sdram+(pc&0x3ffffu));
  unsigned n=0, r0=s->r[0],r6=s->r[6],r4=s->r[4],r8=s->r[8],r2=s->r[2],r1=0,t=s->t_flag,c=0;
  unsigned data[4], tail=0;
  while(n<4 && s->icount>=(int)(8*(n+1)) && (pc&0x3ffffu)+16*(n+1)<=0x40000u){
    const unsigned short *q=p+8*n;
    unsigned dt=0;
#ifdef PIXEL_NATIVE_TAIL
    dt=q[7]==0x4710;
#endif
    if(q[0]!=0x612c || q[1]!=0x4218 || q[2]!=0x212b || q[3]!=0x2015 || q[4]!=0x38de || q[5]!=0x6240 || q[6]!=0x34ee || (q[7]!=0x2615 && !dt))break;
    unsigned a=r0-2,b=r6-2;
    if((a&0xdf000000u)!=0x04000000u || (b&0xdf000000u)!=0x04000000u || ((a|b)&1) || (r4&0xdf000000u)!=0x06000000u)break;
    r1=(r2&255)|(r2<<8);data[n]=r1&65535;
    unsigned old=r8,tmp=old+s->r[13];r8=tmp+t;t=(old>tmp)||(tmp>r8);
    c=r4;r2=(unsigned)(int)(signed char)((unsigned char *)s->p_sdram)[(c&0x3ffffu)^1];
    old=c;tmp=old+s->r[14];r4=tmp+t;t=(old>tmp)||(tmp>r4);
    r0=a;if(!dt)r6=b;n++;
    if(dt){tail=1;break;}
  }
  if(!n)return 0;
#ifdef PIXEL_SHADOW_PROOF
  static int guards_checked;
  if(!guards_checked){
    guards_checked=1;
    for(unsigned i=0;i<12;i++){
      SH2 q=*s, saved;
      unsigned qpc=pc;
      switch(i){
      case 0:q.delay=pc;break;
      case 1:q.test_irq=1;break;
      case 2:q.icount=7;break;
      case 3:q.p_sdram=0;break;
      case 4:q.p_dram=0;break;
      case 5:q.r[0]=0x20004002;break;
      case 6:q.r[6]=0x20004002;break;
      case 7:q.r[4]=0x20004000;break;
      case 8:q.r[0]|=1;break;
      case 9:q.r[6]|=1;break;
      case 10:qpc=0x0603fff2;break;
      case 11:qpc=0x02000000;break;
      }
      saved=q;
      if(pixel_native(&q,qpc) || memcmp(&q,&saved,sizeof(q))){printf("PIXEL_GUARD_FAIL %u\n",i);exit(2);}
      pixel_declines++;
    }
  }
  SH2 before,reference;
  unsigned short *slots[8],values[8],expected[8];
  if(pixel_proofs<2048){
    for(unsigned i=0;i<n;i++){
      slots[2*i]=(unsigned short *)s->p_dram+(((s->r[0]-2*(i+1))&0x1ffffu)>>1);
      slots[2*i+1]=(unsigned short *)s->p_dram+(((s->r[6]-2*(i+1))&0x1ffffu)>>1);
    }
    for(unsigned i=0;i<2*n;i++)values[i]=*slots[i];
    before=*s;reference=*s;reference.pc=pc;reference.icount=8*n;
    pixel_proof_active=1;
    sh2_execute_interpreter(&reference,8*n);
    pixel_proof_active=0;
    for(unsigned i=0;i<2*n;i++)expected[i]=*slots[i];
    for(unsigned i=0;i<2*n;i++)*slots[i]=values[i];
    reference.icount=before.icount-8*n;
  }
#endif
  for(unsigned i=0;i<n;i++){
    pixel_store(s,s->r[0]-2*(i+1),data[i]);
    if(!tail || i+1<n)pixel_store(s,s->r[6]-2*(i+1),data[i]);
  }
  s->r[0]=r0;s->r[1]=r1;s->r[2]=r2;s->r[4]=r4;s->r[6]=r6;s->r[8]=r8;s->t_flag=t;
  if(tail){s->r[7]--;s->t_flag=s->r[7]==0;s->no_polling=SH2_NO_POLLING;}
  s->ea=c;s->pc=pc+16*n;s->ppc=s->pc-2;s->icount-=8*n;
#ifdef PIXEL_SHADOW_PROOF
  if(pixel_proofs<2048){
    unsigned bad=0;for(unsigned i=0;i<2*n;i++)bad|=*slots[i]!=expected[i];
    if(memcmp(s,&reference,sizeof(*s)) || bad){
      printf("PIXEL_PROOF_FAIL pc=%08x context=%d memory=%d\n",pc,memcmp(s,&reference,sizeof(*s)),bad);
      exit(2);
    }
    pixel_proofs++;
  }
#endif
  pixel_calls++;pixel_groups+=n;return 8*n;
}
