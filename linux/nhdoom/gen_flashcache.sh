#!/usr/bin/env bash
# Generate a false-positive-free /roms/homebrew/doom1.flashcache.bin from the
# converted IWAD: bake the host harness several times (each dumps its built flash
# cache + a value-range reloc table), then filter to the reloc entries that are
# consistent across bakes -- which are exactly the real pointers (a data word that
# only coincidentally looks like a pointer at one ASLR base is dropped).
#
# The bake exits the moment the cache is built (NHDOOM_BAKE_HOOK), BEFORE the
# host engine's layout-flaky first frame, so a clean bake is reliable; we still
# retry past the occasional crash during the cache build itself.
#
# Usage: gen_flashcache.sh <doom1.mcu.wad> <out doom1.flashcache.bin> [nbakes]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WAD="${1:?usage: gen_flashcache.sh <mcu.wad> <out> [nbakes]}"
OUT="${2:?need output path}"
N="${3:-3}"
ELF="$HERE/../build_nhdoom/nhdoom.elf"
[ -x "$ELF" ] || { echo "build first: make -C linux -f Makefile.nhdoom"; exit 1; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
ulimit -c 0 2>/dev/null
bakes=()
for n in $(seq 1 "$N"); do
  f="$TMP/bake$n.bin"
  for t in $(seq 1 200); do
    NHDOOM_BAKE="$f" timeout 16 "$ELF" "$WAD" >/dev/null 2>/dev/null
    [ -s "$f" ] && break
  done
  [ -s "$f" ] || { echo "bake $n never succeeded"; exit 1; }
  echo "bake $n ok ($(stat -c%s "$f") bytes)"
  bakes+=("$f")
done
python3 "$HERE/build_flashcache.py" "$OUT" "${bakes[@]}"
echo "wrote $OUT"
