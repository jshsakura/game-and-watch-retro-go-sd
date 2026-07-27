#!/usr/bin/env python3
"""Regenerate thumbs.bin for the 320x240 gallery.

Upstream cuts 40x40 thumbnails for a 466x466 panel and the firmware loads the
whole file into RAM (169 KB). Our gallery grid is 4x4 inside 320x240, so the
cells are roughly half as wide and the extra detail is paid for and never seen.

The upstream generator is parameterised by exactly two module globals, so this
overrides those and reuses its algorithm rather than reimplementing the packing
-- the TPTH layout stays whatever upstream says it is.

  python3 make_thumbs_320.py <mons_dir> [cell]
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CELL = 24

# Upstream's generator is not vendored -- the sprites it packs are CC BY-NC, and
# keeping the toolchain pointed at a local checkout is what stops any of that
# material drifting into this tree. Point TAMAPOKE_UPSTREAM at a clone of
# https://github.com/socquique/TamaPoke.
UPSTREAM = os.environ.get(
    'TAMAPOKE_UPSTREAM_MAKE_THUMBS',
    os.path.join(os.environ.get('TAMAPOKE_UPSTREAM', ''), 'tools', 'make_thumbs.py'),
)


def load_upstream():
    if not UPSTREAM or not os.path.exists(UPSTREAM):
        raise SystemExit(
            'cannot find upstream make_thumbs.py at %r\n'
            'set TAMAPOKE_UPSTREAM to a checkout of github.com/socquique/TamaPoke'
            % UPSTREAM)
    spec = importlib.util.spec_from_file_location('make_thumbs', UPSTREAM)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    mons_dir = sys.argv[1]
    cell = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_CELL

    mod = load_upstream()
    mod.DIR = mons_dir
    mod.CELL = cell
    mod.main()


if __name__ == '__main__':
    main()
