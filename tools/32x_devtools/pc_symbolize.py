"""Symbolize raw SysTick PC samples of one run: per function, per region, per 64 B line."""
import sys,struct,subprocess,bisect,json,collections
from pathlib import Path
elf=sys.argv[1];runs=[Path(a) for a in sys.argv[2:]];run=runs[0];XIP_LINK=0xDEB00000
samples=[];tot=hx=drp=cnt=capn=0
for r in runs:
    cap=r/'capture'
    mg,t,h,d,c,cp,buf,stride=struct.unpack('<8I',(cap/'pc-record.bin').read_bytes())
    tot+=t;hx+=h;drp+=d;cnt+=c;capn+=cp
    x=json.loads((cap/'placement.json').read_text())['xip_addr']
    samples+=[(p&~1,x) for (p,) in struct.iter_unpack('<I',(cap/'pc-samples.bin').read_bytes())]
pcs=samples;xip=None
sec={};secaddr={}
for l in subprocess.run(['readelf','-SW',elf],capture_output=True,text=True).stdout.splitlines():
    l=l.replace('[ ','[')
    if l.strip().startswith('[') and ']' in l:
        f=l.split(']')[1].split()
        idx=l.split(']')[0].strip('[ ')
        if len(f)>4 and idx.isdigit(): sec[idx]=f[0];secaddr[f[0]]=(int(f[2],16),int(f[4],16))
want={'.overlay_md32x','.overlay_md32x_itc','.xip_md32x','.rodata_md32x','.text','.itcram_hot','.ram_exec'}
syms=[]
for l in subprocess.run(['readelf','-sW',elf],capture_output=True,text=True).stdout.splitlines():
    f=l.split()
    if len(f)>=8 and f[3]=='FUNC' and f[6].isdigit() and sec.get(f[6]) in want:
        a=int(f[1],16)&~1;syms.append((a,int(f[2]),f[7],sec[f[6]]))
syms.sort();starts=[s[0] for s in syms]
xs=secaddr['.xip_md32x'][1]+secaddr.get('.rodata_md32x',(0,0))[1]
def region(p,xip):
    if xip<=p<xip+xs: return 'XIP',p-xip+XIP_LINK
    if p<0x10000: return 'ITCM',p
    if 0x08000000<=p<0x08200000: return 'INTFLASH',p
    if 0x24000000<=p<0x24100000: return 'RAM_EMU/AXI',p
    if 0x20000000<=p<0x20020000: return 'DTCM',p
    return 'OTHER',p
def fn(a):
    i=bisect.bisect_right(starts,a)-1
    if i>=0 and a<syms[i][0]+max(syms[i][1],2): return syms[i][2]
    return '?'
R=collections.Counter();F=collections.Counter();FR={};L=collections.Counter()
for p,x in pcs:
    r,a=region(p,x);R[r]+=1;n=fn(a);F[n]+=1;FR[n]=r;L[(r,a&~63,n)]+=1
N=len(pcs)
out=dict(stride=stride,total=tot,handler_excluded=hx,dropped=drp,count=cnt,capacity=capn,runs=[str(r) for r in runs],
 regions={k:round(100*v/N,2) for k,v in R.most_common()},
 functions=[dict(fn=k,region=FR[k],pct=round(100*v/N,2),n=v) for k,v in F.most_common()],
 lines=[dict(region=r,addr=hex(a),fn=n,pct=round(100*v/N,2)) for (r,a,n),v in L.most_common(40)])
(run/'pc-profile.json').write_text(json.dumps(out,indent=1))
print(json.dumps({k:out[k] for k in ['stride','total','handler_excluded','dropped','count','capacity','regions']}))
for f in out['functions']: print(f"{f['pct']:6.2f}% {f['region']:11s} {f['fn']}")
