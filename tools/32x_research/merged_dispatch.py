"""Place native eligibility checks in existing opcode cases, avoiding common gates."""
import re

def inject(core, handlers, args):
    routes=[]
    if args.lookup_native: routes.append(('op0000','case 0x0d:',0x010d,'lookup_native(sh2,sh2->ppc)'))
    if args.control_native:
        routes.extend([('op0010','case 15:',0x201f,'control_native(sh2,sh2->ppc,0)'),('op0011','case  8:',0x3d88,'control_native(sh2,sh2->ppc,1)'),('op0101',None,0x52e1,'control_native(sh2,sh2->ppc,2)')])
    if args.chain_native: routes.append(('op0101',None,0x50e6,'chain_native(sh2,sh2->ppc)'))
    if args.pixel_native: routes.append(('op0110','case 12:',0x612c,'pixel_native(sh2,sh2->ppc)'))
    if args.pcm_tail_native: routes.append(('op1101',None,0xd21c,'pcm_tail_native(sh2,sh2->ppc)'))
    names=sorted({r[0] for r in routes})
    core=core.replace('#include "sh2.c"','static unsigned native_gateway(SH2 *sh2,unsigned opcode);\n'+('static unsigned countdown_fold(SH2 *s);\n' if args.countdown_fold else '')+'#include "sh2.c"',1)
    gateway=['static unsigned native_gateway(SH2 *sh2,unsigned opcode){','switch(opcode){']
    for _,_,op,call in routes:gateway.append(f'case 0x{op:04x}: return {call};')
    gateway+=['default:return 0;','}}']
    signature='int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
    core=core.replace(signature,'\n'.join(gateway)+'\n'+signature,1)
    for name in names:
        match=re.search(r'INLINE void '+name+r'\(.*?\n\}',handlers,re.S);assert match,name
        body=match.group().replace('INLINE void','INLINE unsigned',1).replace('UINT16 opcode)','UINT16 opcode, int can_native)',1)
        for route,anchor,op,_ in routes:
            if route!=name:continue
            gate=f' if(can_native && opcode==0x{op:04x} && native_gateway(sh2,opcode))return 1;'
            if anchor is None:body=body.replace('\n{','\n{'+gate,1)
            else:
                assert body.count(anchor)==1,(name,anchor)
                body=body.replace(anchor,anchor+gate,1)
        body=body[:-1]+'\treturn 0;\n}'
        handlers=handlers[:match.start()]+body+handlers[match.end():]
        # Remove the experimental outer gates in this interpreter arm.
        major=int(name[2:],2)
        pattern=r'\t\tcase 0x'+format(major,'X')+r':[^\n]*'+name+r'\(sh2,\s*opcode\)[^\n]*'
        replacement=f'\t\tcase 0x{major:X}: if({name}(sh2,opcode,!rig_is_delay)) {{ RIG_OPCOST_T1(sh2,opcode);continue; }} break;'
        core,count=re.subn(pattern,lambda _:replacement,core,count=1);assert count,(name,pattern)
        # Trace interpreter does not use native blocks.
        core=core.replace(name+'(sh2, opcode);',name+'(sh2, opcode, 0);')
    if args.countdown_fold:
        core=core.replace('\t\tif (opcode==0x8bfa && sh2->pc+8==sh2->ppc) countdown_fold(sh2);\n','')
        handlers=handlers.replace('INLINE void op1000(sh2_state *sh2, UINT16 opcode)','INLINE void op1000(sh2_state *sh2, UINT16 opcode, int can_native)',1)
        anchor='case 11<< 8: BF(sh2, opcode & 0xff);'
        assert handlers.count(anchor)==1
        handlers=handlers.replace(anchor,anchor+' if(can_native && opcode==0x8bfa && sh2->pc+8==sh2->ppc && !sh2->test_irq){sh2->icount--;countdown_fold(sh2);sh2->icount++;}',1)
        core=core.replace('op1000(sh2, opcode);','op1000(sh2, opcode, !rig_is_delay);',1)
        core=core.replace('op1000(sh2, opcode);','op1000(sh2, opcode, 0);')
    return core,handlers
