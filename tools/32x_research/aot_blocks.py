#!/usr/bin/env python3
"""Automatically select and generate guarded SH2 blocks, independent of title.

Direct calculations and live C locals are lowered to Thumb by the target GCC.
Profiles are input data; the code generator has no ROM names or fixed PCs.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
from native_blocks import decode as alu_decode

# Preserve every existing handler-level shortcut. These are interpreter entry
# opcodes, not guest addresses. Executing their bare primitive can change the
# reference's bookkeeping or forfeit its already-measured native path.
EXISTING_NATIVE = {0x010d,0x201f,0x3d88,0x52e1,0x50e6,0x612c,0x6418,0x6316,0x6616,0xd21c,0xd834}

def decode(op, offset):
    if op in EXISTING_NATIVE:
        raise ValueError('existing native entry')
    n,m=(op>>8)&15,(op>>4)&15
    try:
        code,touched,dirty,flag=alu_decode(op)
        return dict(code=code,touched=touched,dirty=dirty,flag=flag,read=False,ea=False)
    except ValueError:
        pass
    key=op&0xf00f
    if key in (0x0007,0x200e,0x200f,0x3005,0x300d):
        if key==0x0007:code=f'macl=r{n}*r{m};'
        elif key==0x200e:code=f'macl=(uint32_t)(uint16_t)r{n}*(uint16_t)r{m};'
        elif key==0x200f:code=f'macl=(uint32_t)((int32_t)(int16_t)r{n}*(int16_t)r{m});'
        else:
            product=f'(uint64_t)r{n}*r{m}' if key==0x3005 else f'(uint64_t)((int64_t)(int32_t)r{n}*(int32_t)r{m})'
            code=f'{{uint64_t z={product};macl=(uint32_t)z;mach=(uint32_t)(z>>32);}}'
        return dict(code=code,touched={n,m},dirty=set(),flag=False,read=False,ea=False,
                    cycles=1 if key in (0x200e,0x200f) else 2,
                    aux={'macl','mach'} if key in (0x3005,0x300d) else {'macl'})
    if op&0xf0ff in (0x000a,0x001a):
        field='mach' if op&0xf0ff==0x000a else 'macl'
        return dict(code=f'r{n}={field};',touched={n},dirty={n},flag=False,read=False,ea=False,aux_read={field})
    width=None; address=None; post=False; ea=True; touched={n,m}; dirty={n}
    if key in (0x6000,0x6001,0x6002,0x6004,0x6005,0x6006):
        width=1<<(key&3);address=f'r{m}';post=bool(key&4);ea=not post
        if post and n!=m:dirty.add(m)
    elif key in (0x000c,0x000d,0x000e):
        width=1<<((key&15)-12);address=f'r0+r{m}';touched.add(0)
    elif op>>12==5:
        width=4;address=f'r{m}+{(op&15)*4}u'
    elif op>>12 in (9,13):
        width=2 if op>>12==9 else 4;touched={n}
        address=f'pc+{offset+4+(op&255)*2}u' if width==2 else f'((pc+{offset+4}u)&~3u)+{(op&255)*4}u'
    elif op&0xff00 in (0x8400,0x8500):
        width=1 if op&0xff00==0x8400 else 2;n=0;dirty={0};touched={0,m}
        address=f'r{m}+{(op&15)*width}u'
    elif op&0xff00 in (0xc400,0xc500,0xc600):
        width=1<<((op>>8)-0xc4);n=0;dirty={0};touched={0};address=f's->gbr+{(op&255)*width}u'
    elif op&0xff00==0xc700:
        return dict(code=f'ea=((pc+{offset+4}u)&~3u)+{(op&255)*4}u; r0=ea;',touched={0},dirty={0},flag=False,read=False,ea=True)
    if width is not None:
        cast='(uint32_t)(int32_t)(int8_t)' if width==1 else '(uint32_t)(int32_t)(int16_t)' if width==2 else ''
        code=f'{{ uint32_t a={address},v; if(!aot_read(s,a,{width},&v)) {{ done={offset//2}; goto finish; }} '
        if ea:code+='ea=a; '
        code+=f'r{n}={cast}v; '
        if post and n!=m:code+=f'r{m}+={width}; '
        code+='}'
        return dict(code=code,touched=touched,dirty=dirty,flag=False,read=True,ea=ea)
    # No delayed branches, stores, SR mask changes or polling shortcuts in a
    # block. The outer interpreter handles these at their exact instruction.
    raise ValueError(f'unsupported {op:04x}')

def collect(paths, limit, minimum):
    candidates=defaultdict(int)
    for path in paths:
        for run in json.loads(path.read_text()):
            if run['delay']:continue
            start=0;ops=run['ops']
            while start<len(ops):
                end=start
                while end<len(ops):
                    try:decode(ops[end],(end-start)*2)
                    except ValueError:break
                    end+=1
                if end-start>=minimum:
                    candidates[(run['pc']+2*start,tuple(ops[start:end]))]+=run['count']
                start=max(end+1,start+1)
    ranked=sorted(candidates.items(),key=lambda item:(-(len(item[0][1])-3)*item[1],item[0]))
    selected=[];pcs=set()
    for (pc,ops),count in ranked:
        # One variant per PC in this bounded first implementation. Exact
        # mismatches fall back, so profiles never license stale execution.
        if pc in pcs:continue
        selected.append(dict(pc=pc,ops=list(ops),observed_count=count));pcs.add(pc)
        if len(selected)==limit:break
    return selected

def emit(blocks, out):
    size=1
    while size<max(128,len(blocks)*16):size*=2
    seed=None
    for trial in range(1,10001,2):
        mult=((0x9e3779b1*trial)^0x85ebca6b)&0xffffffff|1
        slots=[((((b['pc']>>1)*mult)&0xffffffff)>>(32-(size.bit_length()-1))) for b in blocks]
        if len(set(slots))==len(slots):seed=mult;break
    if seed is None:raise ValueError('No collision-free bounded index')
    header='''#include <stdint.h>
#include <cpu/sh2/sh2.h>
struct aot_entry { uint32_t pc,n,cycles; const uint16_t *ops; unsigned (*run)(SH2*,unsigned); };
extern const struct aot_entry *const aot_index[AOT_INDEX_SIZE];
extern const uint8_t aot_opflags[65536];
unsigned aot_enter(SH2*,const struct aot_entry*);
static inline unsigned aot_probe(SH2 *s) {
  const struct aot_entry *e=aot_index[((s->ppc>>1)*AOT_HASH)>>(32-AOT_INDEX_BITS)];
  if(!e || e->pc!=s->ppc) return 0;
  return aot_enter(s,e);
}
'''
    header=f'#define AOT_BLOCK_COUNT {len(blocks)}\n#define AOT_INDEX_SIZE {size}\n#define AOT_INDEX_BITS {size.bit_length()-1}\n#define AOT_HASH {seed}u\n'+header
    (out/'native_module.h').write_text(header)
    code='''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_module.h"
struct gnw_fetch_win { unsigned region,mask; unsigned char *base; };
extern struct gnw_fetch_win gnw_fw_rom;
extern unsigned gnw_sh2_rom_fetch_mask;
#define AOT_CODE __attribute__((noinline,section(".gnw_xip_cold")))
static inline int aot_read(SH2 *s,uint32_t a,unsigned width,uint32_t *v) {
  if(a&(width-1))return 0;
  uint32_t h=a&0xdf000000u,off; const unsigned char *p;
  if(h==0x06000000u && s->p_sdram){p=s->p_sdram;off=a&0x3ffffu;}
  else if(h==0x02000000u && h==gnw_fw_rom.region && gnw_fw_rom.base && gnw_sh2_rom_fetch_mask){p=gnw_fw_rom.base;off=a&gnw_fw_rom.mask;}
  else return 0;
  if(width==1)*v=p[off^1u];
  else if(width==2)*v=*(const uint16_t *)(p+off);
  else { uint32_t q=*(const uint32_t *)(p+off);*v=(q<<16)|(q>>16); }
  return 1;
}
'''
    entries=[]; pool=[]
    for i,b in enumerate(blocks):
        ds=[decode(op,j*2) for j,op in enumerate(b['ops'])]
        touched=set().union(*(d['touched'] for d in ds));dirty=set().union(*(d['dirty'] for d in ds))
        flag=any(d['flag'] for d in ds);ea=any(d['ea'] for d in ds)
        aux=set().union(*(d.get('aux',set()) for d in ds));aux_read=aux|set().union(*(d.get('aux_read',set()) for d in ds))
        costs=[d.get('cycles',1) for d in ds];cost=sum(costs)
        f=[f'AOT_CODE static unsigned aot_{i}(SH2 *s,unsigned pc) {{',
           f' unsigned done={len(ds)},spent={cost}; uint32_t t=s->t_flag;']
        if ea:f+=[' uint32_t ea=s->ea;']
        f += [f' uint32_t {field}=s->{field};' for field in sorted(aux_read)]
        f += [f' uint32_t r{r}=s->r[{r}];' for r in sorted(touched)]
        f += [d['code'].replace(f'done={j};',f'done={j}; spent={sum(costs[:j])};') for j,d in enumerate(ds)]
        if any(d['read'] for d in ds):f+=['finish: if(!done)return 0;']
        f += [f' s->r[{r}]=r{r};' for r in sorted(dirty)]
        if flag:f+=[' s->t_flag=t;']
        if ea:f+=[' s->ea=ea;']
        f += [f' s->{field}={field};' for field in sorted(aux)]
        f+=[' s->ppc=pc+2*(done-1);s->pc=pc+2*done;s->icount-=spent;return done;','}']
        code+='\n'.join(f)+'\n'
        b['cycles']=cost;pool+=b['ops'];entries.append((b['pc'],len(b['ops']),cost,len(pool)-len(b['ops']),i))
    code+='static const uint16_t aot_words[]={'+','.join(str(v) for v in pool)+'};\n'
    code+='const struct aot_entry aot_entries[]={\n'+''.join(f'{{{pc}u,{n},{cost},aot_words+{start},aot_{i}}},\n' for pc,n,cost,start,i in entries)+'};\n'
    code+='const struct aot_entry *const aot_index[AOT_INDEX_SIZE]={\n'+''.join(f'[{slot}]=&aot_entries[{i}],\n' for i,slot in enumerate(slots))+'};\n'
    code+='const uint8_t aot_opflags[65536]={'+','.join(f'[{op}]=1' for op in sorted({b['ops'][0] for b in blocks}))+'};\n'
    code+='''AOT_CODE unsigned aot_enter(SH2 *s,const struct aot_entry *e) {
  unsigned pc=s->ppc;
  if(s->delay || s->test_irq || pc&1 || s->icount<(int)e->cycles)return 0;
  unsigned region=pc&0xdf000000u,mask,offset; const unsigned char *base;
  if(region==0x06000000u){base=s->p_sdram;mask=0x3fffe;}
  else if(region==0x02000000u && region==gnw_fw_rom.region && gnw_fw_rom.mask){base=gnw_fw_rom.base;mask=gnw_fw_rom.mask&~1u;}
  else return 0;
  offset=pc&mask;
  if(!base || e->n*2>mask+2-offset)return 0;
  const uint16_t *live=(const uint16_t *)(base+offset);
  for(unsigned j=0;j<e->n;j++)if(live[j]!=e->ops[j])return 0;
#ifdef AOT_SHADOW_PROOF
  SH2 before=*s,reference=*s;
  unsigned used=e->run(s,pc);
  if(used){
    extern int sh2_aot_reference(SH2*,int);
    unsigned spent=before.icount-s->icount;
    reference.pc=pc;sh2_aot_reference(&reference,spent);reference.icount=before.icount-spent;
    if(memcmp(&reference,s,sizeof *s)){
      const uint32_t *a=(const uint32_t *)&reference,*b=(const uint32_t *)s;
      printf("AOT_MISMATCH pc=%08x used=%u\\n",pc,used);
      for(unsigned i=0;i<sizeof *s/4;i++)if(a[i]!=b[i]){printf("word=%u ref=%08x actual=%08x\\n",i,a[i],b[i]);break;}
      exit(5);
    }
  }
#else
  unsigned used=e->run(s,pc);
#endif
#ifdef RIG_SH2_COUNT
  extern unsigned long long g_sh2_insns;
  if(used)g_sh2_insns+=used-1;
#endif
  return used;
}
'''
    (out/'native_module.c').write_text(code)
    save=dict(blocks=blocks,block_count=len(blocks),instruction_count=len(pool),index_slots=size,index_bytes_m7=size*4,
              semantic_scope='ALU, explicit one/two-cycle multiplies, and safe aligned ROM/SDRAM reads. No stores or branch execution. Interpreter retains branch/delay/IRQ and existing native paths.',
              profile_rule='Rank every eligible observed run by count*(length-3), no title/PC selection in generator. One exact-guarded variant per PC.',
              entry_major_nibbles=sorted({b['ops'][0]>>12 for b in blocks}))
    (out/'blocks.json').write_text(json.dumps(save,indent=2)+'\n')
    return save

def integrate(out, dispatch='opcode'):
    meta=json.loads((out/'blocks.json').read_text())
    path=out/'candidate/cpu/sh2/mame/sh2pico.c';text=path.read_text()
    anchor='int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
    assert text.count(anchor)==2
    text=text.replace(anchor,'#include "native_module.h"\n'+anchor,1)
    # Insert at the dispatch, before any handler executes. Do not touch delay
    # slots; the current opcode has already been charged to the guest counter.
    anchor='#else\n\t\tswitch ((opcode >> 12) & 0xf)\n\t\t{'
    assert text.count(anchor)==1
    majors=meta['entry_major_nibbles']
    cond=' || '.join(f'(opcode>>12)=={n}' for n in majors)
    gate=f'({cond})' if dispatch=='all' else 'aot_opflags[opcode]'
    replacement=f'''#else
        if({gate} && gnw_direct && aot_probe(sh2)) {{ RIG_OPCOST_T1(sh2,opcode); continue; }}
\t\tswitch ((opcode >> 12) & 0xf)
\t\t{{'''
    text=text.replace(anchor,replacement,1);path.write_text(text)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--runs',type=Path,action='append',required=True);p.add_argument('--limit',type=int,default=128)
    p.add_argument('--minimum',type=int,default=6)
    p.add_argument('--dispatch',choices=['all','opcode'],default='opcode')
    a=p.parse_args();b=collect(a.runs,a.limit,a.minimum);meta=emit(b,a.output);integrate(a.output,a.dispatch)
    print(json.dumps({k:v for k,v in meta.items() if k!='blocks'}))

if __name__=='__main__':main()
