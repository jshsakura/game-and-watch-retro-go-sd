#!/usr/bin/env python3
"""Read an existing GNU ld map; never connect to a device or build firmware."""
import argparse
import hashlib
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('map', type=Path)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
data = args.map.read_bytes()
text = data.decode(errors='replace')
names = ['__RAM_EMU_END__', '_OVERLAY_MD32X_BSS_END',
         '__md32x_itc_start__', '__md32x_itc_end__',
         '__md32x_itc_bss_start__', '__md32x_itc_bss_end__',
         '_heap_start', '_heap_end', '_Min_Stack_Size',
         '__ahbram_heap_start__', '__ahbram_audio_start__']
symbols = {}
for name in names:
    values = set(int(value, 16) for value in re.findall(
        r'^\s*(0x[0-9a-fA-F]+)\s+' + re.escape(name) + r'\s*=', text, re.M))
    if len(values) != 1:
        raise ValueError(f'{name}: expected one address, found {values}')
    symbols[name] = values.pop()
itcm_region = re.search(r'^ITCMRAM\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)', text, re.M)
if not itcm_region:
    raise ValueError('ITCMRAM memory region missing')
itcm_limit = sum(int(v, 16) for v in itcm_region.groups())
span = lambda end, start: symbols[end] - symbols[start]
result = {
    'kind': 'static_link_map_only',
    'map': str(args.map.resolve()),
    'map_sha256': hashlib.sha256(data).hexdigest(),
    'symbols': {name: hex(value) for name, value in symbols.items()},
    'bytes': {
        'axi_static_margin': span('__RAM_EMU_END__', '_OVERLAY_MD32X_BSS_END'),
        'itcm_code': span('__md32x_itc_end__', '__md32x_itc_start__'),
        'itcm_data': span('__md32x_itc_bss_end__', '__md32x_itc_bss_start__'),
        'itcm_static_margin': itcm_limit - symbols['__md32x_itc_bss_end__'],
        'dtcm_heap_capacity_before_runtime_allocations': span('_heap_end', '_heap_start'),
        'stack_reserved': symbols['_Min_Stack_Size'],
        'ahb_heap_capacity_before_runtime_allocations': span('__ahbram_audio_start__', '__ahbram_heap_start__'),
    },
    'limitations': [
        'A diagnostic/private map is not a normal firmware memory certification.',
        'Margins are not runtime free memory; allocators, loader and buffers must be accounted for.',
        'DTCM/AHB executable permission and execution cost cannot be inferred from this map.',
        'ITCM data occupies space after code; code-end alone is not the allocation boundary.',
    ],
}
rendered = json.dumps(result, indent=2) + '\n'
if args.output:
    args.output.write_text(rendered)
print(rendered)
