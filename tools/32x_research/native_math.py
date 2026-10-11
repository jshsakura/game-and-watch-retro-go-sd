"""Replace software partial products with native wide multiplies in snapshots."""
import re

def inject(text):
    for name in ('DMULS','DMULU','MAC_L'):
        match=re.search(r'INLINE void '+name+r'\(.*?\n\}',text,re.S)
        assert match
        old=match.group()
        if name!='MAC_L':
            product='(unsigned long long)((long long)(INT32)sh2->r[n]*(INT32)sh2->r[m])' if name=='DMULS' else '(unsigned long long)sh2->r[n]*sh2->r[m]'
            new=f'''INLINE void {name}(sh2_state *sh2, UINT32 m, UINT32 n)
{{
  unsigned long long z={product};
  sh2->macl=(UINT32)z;sh2->mach=(UINT32)(z>>32);sh2->icount--;
}}'''
        else:
            suffix=old[old.index('\tif (sh2->sr & S)'):]
            new='''INLINE void MAC_L(sh2_state *sh2, UINT32 m, UINT32 n)
{
  UINT32 Res0,Res2;
  INT32 tempn=(INT32)RL(sh2,sh2->r[n]);sh2->r[n]+=4;
  INT32 tempm=(INT32)RL(sh2,sh2->r[m]);sh2->r[m]+=4;
  unsigned long long z=(unsigned long long)((long long)tempn*tempm);
  Res0=(UINT32)z;Res2=(UINT32)(z>>32);
'''+suffix
        legacy=old.replace('void '+name+'(', 'void legacy_'+name+'(',1)
        text=text[:match.start()]+'#ifdef NATIVE_MATH_PROOF\n'+legacy+'\n#endif\n'+new+text[match.end():]
    return text

PROOF=r'''
#include <string.h>
#include <stdlib.h>
void native_math_proof(SH2 *base){
#ifdef NATIVE_MATH_PROOF
  unsigned seed=0x32a07,checks=0;
  static const unsigned edge[]={0,1,2,0x7fff,0x8000,0xffff,0x10000,0x7fffffff,0x80000000,0xffffffff};
  unsigned ram[4];
  for(unsigned i=0;i<8192;i++){
    seed=seed*1664525u+1013904223u;unsigned x=i<100?edge[i/10]:seed;
    seed=seed*1664525u+1013904223u;unsigned y=i<100?edge[i%10]:seed;
    for(unsigned j=0;j<4;j++){unsigned v=j&1?y:x;ram[j]=(v<<16)|(v>>16);}
    for(unsigned op=0;op<3;op++)for(unsigned alias=0;alias<2;alias++){
      SH2 a=*base,b;
      a.p_sdram=ram;a.sr=(i&1)?S:0;a.mach=i<100?edge[i/10]:seed;
      a.macl=i<100?edge[i%10]:~seed;a.icount=100;
      a.r[0]=op==2?0x06000000u:x;a.r[1]=op==2?0x06000008u:y;b=a;
      unsigned m=alias?0:1;
      if(op==0){legacy_DMULS(&a,m,0);DMULS(&b,m,0);}
      if(op==1){legacy_DMULU(&a,m,0);DMULU(&b,m,0);}
      if(op==2){legacy_MAC_L(&a,m,0);MAC_L(&b,m,0);}
      if(memcmp(&a,&b,sizeof(a))){printf("MATH_PROOF_FAIL case=%u op=%u alias=%u\n",i,op,alias);exit(2);}
      checks++;
    }
  }
  printf("MATH_PROOF checks=%u context=%u\n",checks,(unsigned)sizeof(SH2));
#endif
}
'''
