#!/usr/bin/env python3
"""Prepare one isolated generic arithmetic-span candidate; no hardware access."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]

def prepare(source, output):
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(source, output/'picodrive', ignore=shutil.ignore_patterns('.git','*.o','*.a'))
    path=output/'picodrive/cpu/sh2/mame/sh2pico.c'
    text=path.read_text()
    anchor='int sh2_execute_interpreter(SH2 *sh2, int cycles)\n{'
    assert text.count(anchor)==2
    header=(ROOT/'tools/32x_research/generic_alu_span.h').read_text()
    text=text.replace(anchor, '#ifdef GNW_GENERIC_ALU_SPAN\n'+header+'\n#endif\n'+anchor, 1)
    for major,handler in [(7,'op0111'),(14,'op1110')]:
        anchor=f'\t\tcase 0x{major:X}: {handler}(sh2, opcode); break;'
        assert text.count(anchor)==1,anchor
        replacement=f'''#ifdef GNW_GENERIC_ALU_SPAN
        case 0x{major:X}:
          if(gnw_direct && generic_alu_span(sh2,opcode)) {{ RIG_OPCOST_T1(sh2,opcode); continue; }}
          {handler}(sh2,opcode); break;
#else
{anchor}
#endif'''
        text=text.replace(anchor,replacement,1)
    path.write_text(text)
    manifest={'root_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'picodrive_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip(),
              'source':str(source),'candidate':str(output/'picodrive'),
              'change':'Generic one-cycle pure arithmetic spans, entered from ADDI/MOVI; no game selection or code cache',
              'source_sha256':hashlib.sha256((source/'cpu/sh2/mame/sh2pico.c').read_bytes()).hexdigest(),
              'candidate_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
              'header_sha256':hashlib.sha256(header.encode()).hexdigest(),
              'device_access':False}
    (output/'inputs.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (output/'state.json').write_text(json.dumps({'objective':'One generic 32X optimization attempt',
        'status':'prepared','hypothesis':manifest['change'],'last_result':None,
        'budget':{'hypotheses':1,'titles':2},'device_state':'untouched'},indent=2)+'\n')
    return output/'picodrive'

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'external/picodrive')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    print(prepare(args.source.resolve(),args.output.resolve()))
