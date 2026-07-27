#!/usr/bin/env python3
"""Generate the species-name tables that ship on the card.

English comes out of the user's own upstream checkout; Korean comes from
PokeAPI, which is where upstream already sources its base stats. Neither list
lives in this repository -- they are trademarks, and keeping them out is what
lets the source be shared.

DEX_TBL ships with null name fields and fills them in at startup from the card.

Format (little-endian):
  char[4] "TNAM"
  u16     count
  count x NUL-terminated strings, in dex order starting at #1

  TAMAPOKE_UPSTREAM=/path/to/TamaPoke python3 make_dex_names.py <out_dir> [lang]

lang defaults to "en" (read from upstream) and writes names.bin. Any other
language is fetched from PokeAPI and writes names_<lang>.bin.
"""
import json
import os
import re
import struct
import sys
import time
import urllib.request

MAGIC = b'TNAM'
DEX_COUNT = 151

POKEAPI = 'https://pokeapi.co/api/v2/pokemon-species/%d/'
CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.pokeapi_cache')
FETCH_PAUSE_S = 0.05  # be a polite client; the whole run is 151 requests


def fetch_localised_names(lang):
    """Pull one localised name per species from PokeAPI, caching each response.

    Deliberately not vendored: this is somebody else's data about somebody
    else's trademarks, and the point of fetching it here is that it never has
    to enter our tree."""
    os.makedirs(CACHE_DIR, exist_ok=True)
    names = []
    for dex in range(1, DEX_COUNT + 1):
        cached = os.path.join(CACHE_DIR, '%03d.json' % dex)
        if os.path.exists(cached):
            data = json.load(open(cached, encoding='utf-8'))
        else:
            req = urllib.request.Request(POKEAPI % dex,
                                         headers={'User-Agent': 'tamapoke-gnw-port'})
            data = json.load(urllib.request.urlopen(req, timeout=30))
            json.dump(data, open(cached, 'w', encoding='utf-8'), ensure_ascii=False)
            time.sleep(FETCH_PAUSE_S)
            if dex % 25 == 0:
                print("  fetched %d/%d" % (dex, DEX_COUNT))

        hit = [n['name'] for n in data['names'] if n['language']['name'] == lang]
        if not hit:
            raise SystemExit("#%d has no %r name in PokeAPI" % (dex, lang))
        names.append(hit[0])
    return names


def read_names(upstream):
    """Pull the ordered name list out of upstream's generated dex.h."""
    path = os.path.join(upstream, 'dex.h')
    if not os.path.exists(path):
        raise SystemExit(
            'cannot find %s\n'
            'set TAMAPOKE_UPSTREAM to a checkout of github.com/socquique/TamaPoke' % path)

    src = open(path, encoding='utf-8').read()
    try:
        body = src.split('DEX_TBL[DEX_COUNT + 1] = {', 1)[1].split('};', 1)[0]
    except IndexError:
        raise SystemExit('%s: no DEX_TBL found -- upstream layout changed?' % path)

    names = re.findall(r'\{\s*"([^"]*)"', body)
    if len(names) < DEX_COUNT + 1:
        raise SystemExit('%s: expected %d entries, found %d'
                         % (path, DEX_COUNT + 1, len(names)))
    return names[1:DEX_COUNT + 1]  # entry 0 is the unused placeholder


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out_dir = sys.argv[1]
    lang = sys.argv[2] if len(sys.argv) > 2 else 'en'

    if lang == 'en':
        names = read_names(os.environ.get('TAMAPOKE_UPSTREAM', ''))
        out_name = 'names.bin'
    else:
        print('fetching %s names from PokeAPI...' % lang)
        names = fetch_localised_names(lang)
        out_name = 'names_%s.bin' % lang
    blob = MAGIC + struct.pack('<H', len(names))
    for n in names:
        blob += n.encode('utf-8') + b'\0'

    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, out_name)
    open(out, 'wb').write(blob)
    print('wrote %s: %d names, %d bytes' % (out, len(names), len(blob)))


if __name__ == '__main__':
    main()
