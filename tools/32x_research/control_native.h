/* Guarded native arithmetic/control patterns, independent of title and PC. */
#include <string.h>
#include <stdlib.h>
static unsigned control_calls[3],control_proofs[3],control_taken[3];
static int control_proof_active;
static int control_ram(unsigned a){return (a&0xdf000000u)==0x06000000u;}
static int control_memory(SH2 *s,unsigned a,unsigned width){
  if(control_ram(a))return s->p_sdram!=0;
  return (a&0xdf000000u)==gnw_fw_rom.region && gnw_fw_rom.base && gnw_sh2_rom_fetch_mask && (width!=4 || !(a&3));
}
static unsigned control_rd32(SH2 *s,unsigned a){
  const unsigned char *base;unsigned off;
  if(control_ram(a)){base=s->p_sdram;off=a&0x3fffcu;}
  else{base=gnw_fw_rom.base;off=a&gnw_fw_rom.mask;}
  unsigned v=*(const unsigned *)(base+off);return (v<<16)|(v>>16);
}
static unsigned control_rd8(SH2 *s,unsigned a){
  const unsigned char *base;unsigned off;
  if(control_ram(a)){base=s->p_sdram;off=a&0x3ffffu;}
  else{base=gnw_fw_rom.base;off=a&gnw_fw_rom.mask;}
  return (unsigned)(int)(signed char)base[off^1];
}
static unsigned control_native(SH2 *s,unsigned pc,unsigned kind){
  if(control_proof_active || s->delay || s->test_irq || !s->p_sdram || !control_ram(pc) || (pc&1))return 0;
  const unsigned short *p=(const unsigned short *)((const unsigned char *)s->p_sdram+(pc&0x3ffffu));
  unsigned cycles,branch=0,r0=s->r[0],r1=s->r[1],r2=s->r[2],r3=s->r[3],r8=s->r[8],r13=s->r[13],macl=s->macl,mach=s->mach;
  if(kind==0){
    if(s->icount<11 || (pc&0x3ffffu)>0x3ffecu)return 0;
    static const unsigned short ops[]={0x201f,0x4129,0x021a,0x201f,0x3c2c,0x021a,0x3b2c,0x4710,0x8fd7,0x7e20};
    for(unsigned i=0;i<10;i++)if(p[i]!=ops[i])return 0;
    r2=(unsigned)((int)(short)r0*(short)r1);r1>>=16;
    macl=(unsigned)((int)(short)r0*(short)r1);
    branch=s->r[7]!=1;cycles=10+branch;
  } else {
    if(s->icount<(kind==1?23:15) || (pc&0x3ffffu)>(kind==1?0x3ffd6u:0x3ffe4u))return 0;
    static const unsigned short ops[]={0x3d88,0x3d1d,0x3d8c,0x51e4,0x001a,0x3012,0x890d,0x52e1,0x680d,0x4029,0x302c,0x6304,0x4819,0x6004,0x3038,0x208f,0x51e0,0x001a,0x4019,0xa004,0x303c};
    unsigned skip=kind==1?0:7;
    for(unsigned i=0;i<21-skip;i++)if(p[i]!=ops[i+skip])return 0;
    if(kind==1){
      unsigned a=s->r[14]+16;
      if(!control_memory(s,a,4))return 0;
      r13-=r8;
      unsigned long long product=(unsigned long long)((long long)(int)r13*(int)r1);
      mach=(unsigned)(product>>32);macl=(unsigned)product;r13+=r8;
      r1=control_rd32(s,a);r0=macl;branch=r0>=r1;
    }
    if(branch)cycles=10;
    else {
      if(!control_memory(s,s->r[14]+4,4) || !control_memory(s,s->r[14],4))return 0;
      r2=control_rd32(s,s->r[14]+4);r8=r0&65535;r0=(r0>>16)+r2;
      if(!control_memory(s,r0,1) || !control_memory(s,r0+1,1))return 0;
      r3=control_rd8(s,r0);
      r0=control_rd8(s,r0+1);
      r8>>=8;r0-=r3;macl=(unsigned)((int)(short)r0*(short)r8);
      r1=control_rd32(s,s->r[14]);r0=(macl>>8)+r3;cycles=kind==1?23:15;
    }
  }
#ifdef CONTROL_SHADOW_PROOF
  SH2 reference;
  if(control_proofs[kind]<2048){
    reference=*s;reference.pc=pc;
    control_proof_active=1;sh2_execute_interpreter(&reference,cycles);control_proof_active=0;
    reference.icount=s->icount-cycles;
  }
#endif
  if(kind==0){
    s->r[1]=r1;s->r[12]+=r2;s->r[2]=macl;s->r[11]+=macl;s->macl=macl;
    s->r[7]--;s->t_flag=!branch;s->no_polling=SH2_NO_POLLING;
    s->r[14]+=32;s->pc=branch?pc-62:pc+20;s->ppc=pc+18;
    if(branch)s->ea=s->pc;
  }else{
    s->r[13]=r13;s->mach=mach;s->macl=macl;s->r[1]=r1;s->r[0]=r0;if(kind==1)s->t_flag=branch;
    if(branch){s->pc=pc+42;s->ppc=pc+12;}
    else{s->r[2]=r2;s->r[3]=r3;s->r[8]=r8;s->pc=pc+(kind==1?50:36);s->ppc=pc+(kind==1?40:26);}
    s->ea=s->pc;
  }
  s->icount-=cycles;
#ifdef CONTROL_SHADOW_PROOF
  if(control_proofs[kind]<2048){
    if(memcmp(s,&reference,sizeof(*s))){
      printf("CONTROL_FAIL kind=%u pc=%08x cycles=%u\n",kind,pc,cycles);
      const unsigned *a=(const unsigned *)s,*b=(const unsigned *)&reference;
      for(unsigned i=0;i<sizeof(*s)/4;i++)if(a[i]!=b[i])printf("DIFF word=%u actual=%08x reference=%08x\n",i,a[i],b[i]);
      exit(2);
    }
    control_proofs[kind]++;
  }
#endif
  control_calls[kind]++;control_taken[kind]+=branch;return cycles;
}
#ifdef CONTROL_SHADOW_PROOF
/* Synthetic inputs use the real core and bus; live game proofs run afterwards. */
void control_synthetic_proof(SH2 *base){
  static unsigned short ram[0x20000];
  static const unsigned short mul[]={0x201f,0x4129,0x021a,0x201f,0x3c2c,0x021a,0x3b2c,0x4710,0x8fd7,0x7e20};
  static const unsigned short coord[]={0x3d88,0x3d1d,0x3d8c,0x51e4,0x001a,0x3012,0x890d,0x52e1,0x680d,0x4029,0x302c,0x6304,0x4819,0x6004,0x3038,0x208f,0x51e0,0x001a,0x4019,0xa004,0x303c};
  static const unsigned edge[]={0,1,2,0x7fff,0x8000,0xffff,0x10000,0x7fffffff,0x80000000,0xffffffff};
  unsigned seed=0x32c04,checks=0,declines=0,branches[3][2]={{0}};
  for(unsigned i=0;i<0x20000;i++)ram[i]=(i*73u)^0x80ffu;
  for(unsigned kind=0;kind<3;kind++){
    memcpy(ram+0x100,kind==0?mul:kind==1?coord:coord+7,kind==0?sizeof(mul):kind==1?sizeof(coord):sizeof(coord)-14);
    for(unsigned i=0;i<4096;i++){
      SH2 a=*base,b,original;unsigned pc=0x06000200u;
      a.p_sdram=ram;a.delay=0;a.test_irq=0;a.pc=pc+2;a.ppc=pc;a.icount=i%32;
      for(unsigned j=0;j<16;j++){seed=seed*1664525u+1013904223u;a.r[j]=i<100?edge[(i+j)%10]:seed;}
      a.t_flag=i&1;a.r[14]=0x06001000u;a.r[7]=i%4==0?1:i%4==1?0:seed;
      unsigned *table=(unsigned *)(ram+0x800);
      table[0]=seed;table[1]=0x00000601; /* CPU_BE2 -> SDRAM 06010000 */
      table[4]=i&1?0xffffffffu:0;
      if(i%16>=1 && i%16<=6)a.icount=64;
      switch(i%16){
      case 1:a.test_irq=1;break;
      case 2:a.delay=pc;break;
      case 3:a.p_sdram=0;break;
      case 4:if(kind)a.r[14]=0x20004000;else pc|=1;break;
      case 5:pc=0x0603fff8u;break;
      case 6:pc=0x02000200u;break;
      }
      original=a;b=a;
      unsigned consumed=control_native(&a,pc,kind);
      if(!consumed){
        if(memcmp(&a,&original,sizeof(a))){printf("CONTROL_DECLINE_FAIL kind=%u case=%u\n",kind,i);exit(2);}
        declines++;continue;
      }
      b.pc=pc;control_proof_active=1;sh2_execute_interpreter(&b,consumed);control_proof_active=0;
      b.icount=original.icount-consumed;
      if(memcmp(&a,&b,sizeof(a))){printf("CONTROL_SYNTH_FAIL kind=%u case=%u\n",kind,i);exit(2);}
      branches[kind][a.t_flag!=0]++;checks++;
    }
  }
  printf("CONTROL_SYNTH checks=%u declines=%u mul_t0=%u mul_t1=%u coord_t0=%u coord_t1=%u interp_t0=%u interp_t1=%u\n",checks,declines,branches[0][0],branches[0][1],branches[1][0],branches[1][1],branches[2][0],branches[2][1]);
  memset(control_calls,0,sizeof(control_calls));memset(control_proofs,0,sizeof(control_proofs));memset(control_taken,0,sizeof(control_taken));
}
#endif
