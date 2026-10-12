"""Offline core-variant selection by ROM opcode patterns, never title/PC."""
import re

FLAGS=('countdown_fold','pixel_native','pixel_tail','control_native','lookup_native','chain_native','inactive_fold','pcm_tail_native')
PATTERNS={
 'pixel_native':[[0x612c,0x4218,0x212b,0x2015,0x38de,0x6240,0x34ee,0x2615],[0x612c,0x4218,0x212b,0x2015,0x38de,0x6240,0x34ee,0x4710]],
 'control_native':[[0x201f,0x4129,0x021a,0x201f,0x3c2c,0x021a,0x3b2c,0x4710,0x8fd7,0x7e20],[0x3d88,0x3d1d,0x3d8c,0x51e4,0x001a,0x3012,0x890d,0x52e1,0x680d,0x4029,0x302c,0x6304,0x4819,0x6004,0x3038,0x208f,0x51e0,0x001a,0x4019,0xa004,0x303c]],
 'lookup_native':[[0x010d,0x30ee,0x2615]],
 'chain_native':[[0x50e6,0x51e3,0x402b,0x58e2]],
 'inactive_fold':[[0xe100,0xe000,0x201f,0x4129,0x021a,0x201f,0x3c2c,0x021a,0x3b2c,0x4710,0x8fd7,0x7e20]],
 'pcm_tail_native':[[0xd21c,0x951c,0x3b2d,0x911e,0x0b0a,0x3b5c,0x3c2d,0x4b15,0x8901,0x31b3,0xeb01,0x8900,0x6b13,0x0c0a,0x3c5c,0x4c15,0x8901,0x31c3,0xec01,0x8900,0x6c13]],
}

def select(rom):
    hits={}
    for name,patterns in PATTERNS.items():
        offsets=[]
        for pattern in patterns:
            blob=b''.join(op.to_bytes(2,'big') for op in pattern)
            start=0
            while True:
                pos=rom.find(blob,start)
                if pos<0:break
                if not pos&1:offsets.append(pos)
                start=pos+2
        hits[name]=sorted(set(offsets))
    offsets=[]
    # Variable registers and forward exit displacement; preserve proof guards.
    for m in re.finditer(b'\x84[\x00-\xff]\x20\x08\x8b[\x00-\x7f][\x40-\x4f]\x10\x8b\xfa',rom):
        if not m.start()&1:offsets.append(m.start())
    hits['countdown_fold']=offsets
    enabled={name:bool(hits.get(name)) for name in FLAGS}
    enabled['pixel_tail']=enabled['pixel_native']
    enabled['chain_native'] &= enabled['control_native']
    enabled['inactive_fold'] &= enabled['chain_native']
    enabled['pcm_tail_native'] &= enabled['control_native']
    eligible=any(enabled.values())
    enabled['native_math']=eligible
    return {'strategy':'ahead_of_time_core_variant_from_opcode_patterns','variant':'native' if eligible else 'baseline',
            'enabled':enabled,'pattern_rom_offsets':hits,
            'limitations':['Static presence is eligibility, not execution coverage; every live native call still checks current code/state/bus.',
                           'ROM-generated code absent from this scan keeps the baseline path; correctness unaffected, possible speed benefit missed.',
                           'Offline variant build only; device loader selection and firmware link/placement remain pending.']}
