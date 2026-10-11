#!/usr/bin/env python3
"""Generate a raw-opcode Thumb SH2 engine with an explicit register contract.

No guest-PC index, native block cache, ROM profile or title-specific execution.
r4=context, r5=PC, r6=cycles, r7=lazy T, r8..10=ROM window, r11=SDRAM.
The unchanged primitive/native/IRQ/delay machinery handles one slow iteration.
The emitter has no per-title data. GCC supplies only the fallback and ABI.
"""
from pathlib import Path
import argparse

RESERVED={0x010d,0x201f,0x230f,0x2f06,0x3d88,0x52e1,0x50e6,0x612c,0x6418,0x6316,0x6616,0xd21c,0xd834}
OFFSETS=('pc','ppc','pr','sr','t_flag','gbr','vbr','mach','macl','p_sdram','icount','ea','delay','test_irq','no_polling')

def supported(op, branches=False, writes=False):
    if op in RESERVED:return False
    major=op>>12;lo=op&15;k=op&0xf0ff
    if major in (5,7,9,13,14):return True
    if major==6:return True
    if major==1:return writes
    if major==2:return lo in (8,9,10,11,13,14,15) or (writes and lo in (0,1,2,4,5,6))
    if major==3:return lo in (0,2,3,5,6,7,8,10,12,13,14,15)
    if major==4:return k in (0x4000,0x4001,0x4004,0x4005,0x4008,0x4009,0x4010,0x4011,0x4015,0x4018,0x4019,0x4020,0x4024,0x4025,0x4028,0x4029)
    if major==0:return op in (8,9,24,40) or k in (0x0002,0x0012,0x0022,0x000a,0x001a,0x002a,0x0029) or lo in (7,12,13,14) or (writes and lo in (4,5,6))
    if major==8:return op&0xff00 in (0x8400,0x8500,0x8800) or (branches and op&0xff00 in (0x8900,0x8b00,0x8d00,0x8f00) and not op&0x80) or (writes and op&0xff00 in (0x8000,0x8100))
    if branches and major in (10,11):return op!=0xaffe
    if major==12:return op&0xff00 in (0xc400,0xc500,0xc600,0xc700,0xc800,0xc900,0xca00,0xcb00) or (writes and op&0xff00 in (0xc000,0xc100,0xc200))
    return False

def emit(out, predecoded=False, branches=False, writes=False):
    lines=['.syntax unified','.thumb','.cpu cortex-m7','#include "sh2_offsets.inc"',
           '.section .text.sh2_execute_interpreter,"ax",%progbits','.p2align 2',
           '.global sh2_execute_interpreter','.type sh2_execute_interpreter,%function','.thumb_func',
           'sh2_execute_interpreter:',
           ' str r1,[r0,#SH2_ICOUNT]',' cmp r1,#0',' bgt .Lstart',' mov r0,r1',' bx lr',
           '.Lstart:',' push {r4-r11,lr}',' sub sp,sp,#4',' mov r4,r0',' mov r6,r1',
           ' ldr r5,[r4,#SH2_PC]',' ldr r7,[r4,#SH2_T_FLAG]',
           ' ldr r0,=gnw_fw_rom',' ldmia r0,{r8-r10}',' ldr r11,[r4,#SH2_P_SDRAM]',
           '.Lresume:',' ldr r0,[r4,#SH2_DELAY]',' ldr r1,[r4,#SH2_TEST_IRQ]',
           ' orrs r0,r0,r1',' bne .Lfallback_unfetched',
           '.Lfetch:',' and r0,r5,#0xdf000000',' cmp r0,r8',' beq .Lfetch_rom',
           ' cmp r0,#0x06000000',' bne .Lfallback_unfetched',
           ' ubfx r0,r5,#1,#17',' ldrh r12,[r11,r0,lsl #1]',' b .Ldecode',
           '.Lfetch_rom:',' and r0,r5,r9',' ldrh r12,[r10,r0]',
           '.Ldecode:',' add r5,r5,#2',' lsr r0,r12,#12']
    def put(*s):lines.extend(s)
    def table(name, labels):
        put(f' tbh [pc,r0,lsl #1]',f'.L{name}_table:')
        for label in labels:put(f' .hword (.L{label}-.L{name}_table)/2')
    table('major',[f'major{i:x}' for i in range(16)])
    def label(s):put(f'.L{s}:')
    def n():put(' ubfx r3,r12,#8,#4')
    def src():put(' ubfx r0,r12,#4,#4',' ldr r0,[r4,r0,lsl #2]')
    def pair():
        n();put(' ldr r0,[r4,r3,lsl #2]',' ubfx r1,r12,#4,#4',' ldr r1,[r4,r1,lsl #2]')
    def store(reg='r0',tail='tail'):put(f' str {reg},[r4,r3,lsl #2]',f' b .L{tail}')
    def bool_t(cond):
        opposite={'eq':'ne','hs':'lo','hi':'ls','ge':'lt','gt':'le','vs':'vc'}[cond]
        put(f' ite {cond}',f' mov{cond} r7,#1',f' mov{opposite} r7,#0')
    def reject(major):
        for op in sorted(RESERVED):
            if op>>12==major:put(f' movw r0,#{op}',' cmp r12,r0',' beq .Lfallback_fetched')
    def subtable(name,labels):put(' and r0,r12,#15');table(name,labels)

    label('major7');n();put(' sxtb r0,r12',' ldr r1,[r4,r3,lsl #2]',' add r0,r0,r1');store()
    label('majore');n();put(' sxtb r0,r12');store()
    label('major3');reject(3)
    subtable('three',['cmp_eq','fallback_fetched','cmp_hs','cmp_ge','fallback_fetched','dmulu','cmp_hi','cmp_gt','sub','fallback_fetched','subc','fallback_fetched','add','dmuls','addc','addv'])
    for name,condition in [('cmp_eq','eq'),('cmp_hs','hs'),('cmp_ge','ge'),('cmp_hi','hi'),('cmp_gt','gt')]:
        label(name);pair();put(' cmp r0,r1');bool_t(condition);put(' b .Ltail')
    for name,insn in [('add','add'),('sub','sub')]:label(name);pair();put(f' {insn} r0,r0,r1');store()
    label('addc');pair();put(' adds r0,r0,r1',' mov r2,#0',' adc r2,r2,#0',' adds r0,r0,r7',' adc r7,r2,#0');store()
    label('subc');pair();put(' subs r0,r0,r1',' mov r2,#0',' sbc r2,r2,r2',' subs r0,r0,r7',' sbc r7,r2,#0',' rsb r7,r7,#0');store()
    label('addv');pair();put(' adds r0,r0,r1');bool_t('vs');store()
    for name,insn in [('dmulu','umull'),('dmuls','smull')]:
        label(name);pair();put(f' {insn} r0,r1,r0,r1',' str r0,[r4,#SH2_MACL]',' str r1,[r4,#SH2_MACH]',' b .Ltail2')

    label('major2');reject(2)
    subtable('two',(['store1','store2','store4','fallback_fetched','pre1','pre2','pre4','fallback_fetched'] if writes else ['fallback_fetched']*8)+['tst','and','xor','or','fallback_fetched','xtrct','muluw','mulsw'])
    if writes:
        for width in (1,2,4):
            label(f'store{width}');pair();put(' str r0,[r4,#SH2_EA]',f' b .Lwrite{width}')
            label(f'pre{width}');pair();put(f' sub r0,r0,#{width}',' str r0,[r4,r3,lsl #2]',f' b .Lwrite{width}')
    for name,insn in [('and','and'),('xor','eor'),('or','orr')]:label(name);pair();put(f' {insn} r0,r0,r1');store()
    label('tst');pair();put(' tst r0,r1');bool_t('eq');put(' b .Ltail')
    label('xtrct');pair();put(' lsr r0,r0,#16',' orr r0,r0,r1,lsl #16');store()
    for name,insn in [('muluw','uxth'),('mulsw','sxth')]:
        label(name);pair();put(f' {insn} r0,r0',f' {insn} r1,r1',' mul r0,r0,r1',' str r0,[r4,#SH2_MACL]',' b .Ltail')

    label('major6');reject(6)
    subtable('six',['loadb','loadw','loadl','mov','postb','postw','postl','not','swapb','swapw','negc','neg','extub','extuw','extsb','extsw'])
    for name,code in [('mov',[]),('not',['mvn r0,r0']),('swapb',['ubfx r1,r0,#0,#16','rev16 r1,r1','bfi r0,r1,#0,#16']),('swapw',['ror r0,r0,#16']),('neg',['rsb r0,r0,#0']),('extub',['uxtb r0,r0']),('extuw',['uxth r0,r0']),('extsb',['sxtb r0,r0']),('extsw',['sxth r0,r0'])]:
        label(name);n();src();put(*[' '+v for v in code]);store()
    label('negc');n();src();put(' mov r2,#0',' subs r0,r2,r0',' sbc r1,r2,r2',' subs r0,r0,r7',' sbc r7,r1,#0',' rsb r7,r7,#0');store()
    for width,suffix,cast in [(1,'b','sxtb'),(2,'w','sxth'),(4,'l',None)]:
        label('load'+suffix);n();src();put(f' bl .Lread{width}',' str r0,[r4,#SH2_EA]')
        if cast:put(f' {cast} r1,r1')
        store('r1')
        label('post'+suffix);n();src();put(f' bl .Lread{width}')
        if cast:put(f' {cast} r1,r1')
        put(' str r1,[r4,r3,lsl #2]',' ubfx r2,r12,#4,#4',' cmp r2,r3',' beq .Ltail',f' add r0,r0,#{width}',' str r0,[r4,r2,lsl #2]',' b .Ltail')

    label('major4');put(' uxtb r0,r12')
    # Match the whole low byte, not just its low nibble: other 4nxx opcodes
    # include system changes, memory writes, jumps, SHAR folds and TAS.
    shifts={0x00:'shll',0x01:'shlr',0x04:'rotl',0x05:'rotr',0x08:'shll2',0x09:'shlr2',0x10:'dt',0x11:'cmppz',0x15:'cmppl',0x18:'shll8',0x19:'shlr8',0x20:'shll',0x24:'rotcl',0x25:'rotcr',0x28:'shll16',0x29:'shlr16'}
    for byte,name in shifts.items():put(f' cmp r0,#{byte}',f' beq .L{name}')
    put(' b .Lfallback_fetched')
    def loadn():n();put(' ldr r0,[r4,r3,lsl #2]')
    for name,insn in [('shll','lsls'),('shlr','lsrs')]:
        label(name);loadn();put(f' {insn} r0,r0,#1',' mov r7,#0',' adc r7,r7,#0');store()
    for count in (2,8,16):
        for prefix,insn in [('shll','lsl'),('shlr','lsr')]:label(prefix+str(count));loadn();put(f' {insn} r0,r0,#{count}');store()
    label('rotl');loadn();put(' lsr r7,r0,#31',' orr r0,r7,r0,lsl #1');store()
    label('rotr');loadn();put(' and r7,r0,#1',' ror r0,r0,#1');store()
    label('rotcl');loadn();put(' lsr r1,r0,#31',' orr r0,r7,r0,lsl #1',' mov r7,r1');store()
    label('rotcr');loadn();put(' and r1,r0,#1',' lsl r7,r7,#31',' orr r0,r7,r0,lsr #1',' mov r7,r1');store()
    label('dt');loadn();put(' subs r0,r0,#1');bool_t('eq');put(' mov r1,#1024',' str r1,[r4,#SH2_NO_POLLING]');store()
    for name,cond in [('cmppz','ge'),('cmppl','gt')]:label(name);loadn();put(' cmp r0,#0');bool_t(cond);put(' b .Ltail')

    label('major0');reject(0);put(' cmp r12,#9',' beq .Ltail',' cmp r12,#8',' beq .Lclrt',' cmp r12,#24',' beq .Lsett',' cmp r12,#40',' beq .Lclrmac',' uxtb r0,r12')
    for byte,field in [(0x02,'SR'),(0x12,'GBR'),(0x22,'VBR'),(0x0a,'MACH'),(0x1a,'MACL'),(0x2a,'PR')]:put(f' cmp r0,#{byte}',f' beq .Lst_{field}')
    put(' cmp r0,#0x29',' beq .Lmovt');subtable('zero',['fallback_fetched']*4+(['index_store1','index_store2','index_store4'] if writes else ['fallback_fetched']*3)+['mull']+['fallback_fetched']*4+['indexedb','indexedw','indexedl','fallback_fetched'])
    if writes:
        for width in (1,2,4):
            label(f'index_store{width}');pair();put(' ldr r2,[r4]',' add r0,r0,r2',' str r0,[r4,#SH2_EA]',f' b .Lwrite{width}')
    label('clrt');put(' mov r7,#0',' b .Ltail');label('sett');put(' mov r7,#1',' b .Ltail')
    label('clrmac');put(' mov r0,#0',' str r0,[r4,#SH2_MACH]',' str r0,[r4,#SH2_MACL]',' b .Ltail')
    label('movt');n();store('r7')
    for field in ('SR','GBR','VBR','MACH','MACL','PR'):
        label('st_'+field);n();put(f' ldr r0,[r4,#SH2_{field}]')
        if field=='SR':put(' bic r0,r0,#1',' orr r0,r0,r7')
        store()
    label('mull');pair();put(' mul r0,r0,r1',' str r0,[r4,#SH2_MACL]',' b .Ltail2')
    for width,suffix,cast in [(1,'b','sxtb'),(2,'w','sxth'),(4,'l',None)]:
        label('indexed'+suffix);n();src();put(' ldr r1,[r4]',' add r0,r0,r1',f' bl .Lread{width}',' str r0,[r4,#SH2_EA]')
        if cast:put(f' {cast} r1,r1')
        store('r1')

    label('major5');reject(5);n();src();put(' and r1,r12,#15',' add r0,r0,r1,lsl #2',' bl .Lread4',' str r0,[r4,#SH2_EA]');store('r1')
    label('major9');n();put(' uxtb r0,r12',' add r0,r5,r0,lsl #1',' add r0,r0,#2',' bl .Lread2',' str r0,[r4,#SH2_EA]',' sxth r1,r1');store('r1')
    label('majord');reject(13);n();put(' add r0,r5,#2',' bic r0,r0,#3',' uxtb r1,r12',' add r0,r0,r1,lsl #2',' bl .Lread4',' str r0,[r4,#SH2_EA]');store('r1')
    label('major8');put(' lsr r0,r12,#8')
    if writes:put(' cmp r0,#0x80',' beq .Ldisp_store1',' cmp r0,#0x81',' beq .Ldisp_store2')
    put(' cmp r0,#0x84',' beq .Ldisp_b',' cmp r0,#0x85',' beq .Ldisp_w',' cmp r0,#0x88',' beq .Lcmpim')
    if branches:
        put(' tst r12,#0x80',' bne .Lfallback_fetched')
        for byte,name in [(0x89,'bt'),(0x8b,'bf'),(0x8d,'bts'),(0x8f,'bfs')]:put(f' cmp r0,#{byte}',f' beq .L{name}')
    put(' b .Lfallback_fetched')
    for width,suffix,cast in [(1,'b','sxtb'),(2,'w','sxth')]:
        label('disp_'+suffix);src();put(' and r1,r12,#15',f' add r0,r0,r1,lsl #{width-1}',' mov r3,#0',f' bl .Lread{width}',' str r0,[r4,#SH2_EA]',f' {cast} r1,r1');store('r1')
    label('cmpim');put(' sxtb r0,r12',' ldr r1,[r4]',' cmp r1,r0');bool_t('eq');put(' b .Ltail')
    label('majorc');put(' lsr r0,r12,#8')
    for byte,name in ([(0xc0,'gbr_store1'),(0xc1,'gbr_store2'),(0xc2,'gbr_store4')] if writes else [])+[(0xc4,'gbr_b'),(0xc5,'gbr_w'),(0xc6,'gbr_l'),(0xc7,'mova'),(0xc8,'tsti'),(0xc9,'andi'),(0xca,'xori'),(0xcb,'ori')]:put(f' cmp r0,#{byte}',f' beq .L{name}')
    put(' b .Lfallback_fetched')
    for width,suffix,cast in [(1,'b','sxtb'),(2,'w','sxth'),(4,'l',None)]:
        label('gbr_'+suffix);put(' uxtb r0,r12',' ldr r1,[r4,#SH2_GBR]',f' add r0,r1,r0,lsl #{width.bit_length()-1}',' mov r3,#0',f' bl .Lread{width}',' str r0,[r4,#SH2_EA]')
        if cast:put(f' {cast} r1,r1')
        store('r1')
    label('mova');put(' add r0,r5,#2',' bic r0,r0,#3',' uxtb r1,r12',' add r0,r0,r1,lsl #2',' str r0,[r4,#SH2_EA]',' str r0,[r4]',' b .Ltail')
    for name,insn in [('andi','and'),('xori','eor'),('ori','orr')]:label(name);put(' uxtb r0,r12',' ldr r1,[r4]',f' {insn} r0,r0,r1',' str r0,[r4]',' b .Ltail')
    label('tsti');put(' uxtb r0,r12',' ldr r1,[r4]',' tst r0,r1');bool_t('eq');put(' b .Ltail')
    if writes:
        label('major1');pair();put(' and r2,r12,#15',' add r0,r0,r2,lsl #2',' str r0,[r4,#SH2_EA]',' b .Lwrite4')
        for width in (1,2,4):
            label(f'gbr_store{width}');put(' uxtb r0,r12',' ldr r1,[r4,#SH2_GBR]',f' add r0,r1,r0,lsl #{width.bit_length()-1}',
                ' str r0,[r4,#SH2_EA]',' ldr r1,[r4]',f' b .Lwrite{width}')
        for width in (1,2):
            label(f'disp_store{width}');src();put(' and r1,r12,#15',f' add r0,r0,r1,lsl #{width-1}',' str r0,[r4,#SH2_EA]',' ldr r1,[r4]',f' b .Lwrite{width}')
    else:label('major1');put(' b .Lfallback_fetched')
    label('majorf');put(' b .Lfallback_fetched')
    if branches:
        # Preserve the existing backward-conditional folds and BRA $ hook.
        # Delayed slots use the complete C step, even with exhausted cycles.
        for name,condition in [('bt','eq'),('bf','ne')]:
            label(name);put(' cmp r7,#0',f' b{condition} .Ltail',' sub r0,r5,#2',' str r0,[r4,#SH2_PPC]',
                ' sxtb r0,r12',' add r5,r5,r0,lsl #1',' add r5,r5,#2',' str r5,[r4,#SH2_EA]',
                ' sub r6,r6,#2',' b .Lbranch_tail')
        for name,condition in [('bts','eq'),('bfs','ne')]:
            label(name);put(' str r5,[r4,#SH2_DELAY]',' add r5,r5,#2',' cmp r7,#0',f' b{condition} .Lslot_tail',
                ' sxtb r0,r12',' add r5,r5,r0,lsl #1',' str r5,[r4,#SH2_EA]',' sub r6,r6,#1',' b .Lslot_tail')
        label('majora');put(' movw r0,#0xaffe',' cmp r12,r0',' beq .Lfallback_fetched',' b .Lbra')
        label('majorb');put(' add r0,r5,#2',' str r0,[r4,#SH2_PR]')
        label('bra');put(' str r5,[r4,#SH2_DELAY]',' sbfx r0,r12,#0,#12',
            ' add r5,r5,r0,lsl #1',' add r5,r5,#2',' str r5,[r4,#SH2_EA]',' sub r6,r6,#1',' b .Lslot_tail')
    else:
        for major in (10,11):label(f'major{major:x}');put(' b .Lfallback_fetched')

    # Direct memory reads reproduce the current core's exact SDRAM masking
    # (word/long reads align down) and ROM window, including byte swapping.
    for width in (1,2,4):
        label(f'read{width}');put(' and r2,r0,#0xdf000000',' cmp r2,r8',f' beq .Lread{width}_rom',' cmp r2,#0x06000000',' bne .Lfallback_fetched')
        if width==1:put(' ubfx r2,r0,#0,#18',' eor r2,r2,#1',' ldrb r1,[r11,r2]')
        else:put(f' ubfx r2,r0,#{width.bit_length()-1},#{19-width.bit_length()}',f' lsl r2,r2,#{width.bit_length()-1}',f' {"ldrh" if width==2 else "ldr"} r1,[r11,r2]')
        if width==4:put(' ror r1,r1,#16')
        put(' bx lr');label(f'read{width}_rom');put(' and r2,r0,r9')
        if width==1:put(' eor r2,r2,#1')
        put(f' {"ldrb" if width==1 else "ldrh" if width==2 else "ldr"} r1,[r10,r2]')
        if width==4:put(' ror r1,r1,#16')
        put(' bx lr')
    def tick():
        put('#ifdef RIG_SH2_COUNT',' ldr r2,=g_sh2_insns',' ldrd r0,r1,[r2]',' adds r0,r0,#1',' adc r1,r1,#0',' strd r0,r1,[r2]','#endif')
    if writes:
        for width in (1,2,4):
            label(f'write{width}')
            if width<4:put(f' {"uxtb" if width==1 else "uxth"} r1,r1')
            put(' str r5,[r4,#SH2_PC]',' sub r2,r5,#2',' str r2,[r4,#SH2_PPC]',
                ' str r7,[r4,#SH2_T_FLAG]',' str r6,[r4,#SH2_ICOUNT]',
                '#ifdef RIG_SH2_COUNT',' ldr r12,=g_sh2_insns',' ldrd r2,r3,[r12]',
                ' adds r2,r2,#1',' adc r3,r3,#0',' strd r2,r3,[r12]','#endif',
                ' mov r2,r4',f' bl p32x_sh2_write{width*8}',' b .Lwrite_tail')
        label('write_tail');put(' ldr r0,[r4,#SH2_ICOUNT]',' sub r0,r0,#1',' str r0,[r4,#SH2_ICOUNT]',
            ' ldr r1,[r4,#SH2_TEST_IRQ]',' cmp r1,#0',' beq .Lfallback_restore',
            ' mov r0,r4',' bl sh2_fixed_irqtail',' b .Lfallback_restore')
    if branches:
        label('slot_tail');tick();put(' sub r6,r6,#1',' b .Lfallback_unfetched')
        label('branch_tail');tick();put(' subs r6,r6,#1',' bgt .Lfetch',
            ' str r5,[r4,#SH2_PC]',' str r7,[r4,#SH2_T_FLAG]',' str r6,[r4,#SH2_ICOUNT]',' b .Lexit')
    label('tail2');put(' sub r6,r6,#1');label('tail');tick();put(' subs r6,r6,#1',' bgt .Lfetch',
        ' str r5,[r4,#SH2_PC]',' sub r0,r5,#2',' str r0,[r4,#SH2_PPC]',' str r7,[r4,#SH2_T_FLAG]',' str r6,[r4,#SH2_ICOUNT]',' b .Lexit')
    label('fallback_fetched')
    if predecoded:
        put(' str r5,[r4,#SH2_PC]',' sub r0,r5,#2',' str r0,[r4,#SH2_PPC]',
            ' str r7,[r4,#SH2_T_FLAG]',' str r6,[r4,#SH2_ICOUNT]',
            ' mov r0,r4',' mov r1,r12',' bl sh2_fixed_decoded',' b .Lfallback_restore')
    else:put(' sub r5,r5,#2')
    label('fallback_unfetched')
    put(' str r5,[r4,#SH2_PC]',' str r7,[r4,#SH2_T_FLAG]',' mov r0,r4',' mov r1,r6',' bl sh2_fixed_step',
        '.Lfallback_restore:',' mov r6,r0',' ldr r5,[r4,#SH2_PC]',' ldr r7,[r4,#SH2_T_FLAG]',
        ' cmp r6,#0',' bgt .Lresume',' ldr r0,[r4,#SH2_DELAY]',' cmp r0,#0',' bne .Lfallback_unfetched')
    label('exit');put(' mov r0,r6',' add sp,sp,#4',' pop {r4-r11,pc}',
        '.size sh2_execute_interpreter,.-sh2_execute_interpreter','.ltorg')
    out.write_text('\n'.join(lines)+'\n')

def step_source(source, predecoded=False, writes=False):
    """Retain exactly one original outer iteration, including native continues.

    A delayed branch can exhaust the budget before its slot; only the top-level
    assembly entry enforces the initial nonpositive-budget exit. Internal slot
    continuation must still run, as it does in the original outer loop.
    """
    start=source.index('int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{')
    end=source.index('\n#else // if DRC_CMP',start)
    part=source[start:end]
    original_part=part
    part=part.replace('int sh2_execute_interpreter(', '__attribute__((section(".gnw_xip_cold"))) int sh2_fixed_step(',1)
    part=part.replace('\tif (sh2->icount <= 0)\n\t\tgoto out;','',1)
    needle='while (sh2->icount > 0 || sh2->delay);'
    assert part.count(needle)==1
    part=part.replace(needle,'while (0);',1)
    if predecoded:
        body=original_part[original_part.index('\t\tRIG_SH2_TICK();'):]
        body=body[:body.index('\n\twhile (sh2->icount > 0 || sh2->delay);')]
        # body includes the closing brace of the original do-loop. The fetch
        # and PC increment were already executed by the Thumb dispatcher.
        part+='''\n__attribute__((section(".gnw_xip_cold")))
int sh2_fixed_decoded(SH2 *sh2, unsigned opcode) {
 unsigned gnw_direct=1;
#ifdef RIG_SH2_PC_HIST
 int rig_is_delay=0;
#endif
 do {
'''+body+'''\n while(0);
 return sh2->icount;
}
'''
    if writes:
        part+='''
__attribute__((section(".gnw_xip_cold")))
int sh2_fixed_irqtail(SH2 *s) {
 if(s->test_irq && !s->delay) {
  int level=s->pending_level;
  if(level>((s->sr>>4)&15)) {
   int vector=s->irq_callback(s,level);sh2_do_irq(s,level,vector);
  }
  s->test_irq=0;
 }
 return s->icount;
}
'''
    return source[:start]+part+source[end:]

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    emit(p.parse_args().output)
