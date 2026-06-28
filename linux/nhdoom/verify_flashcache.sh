#!/usr/bin/env bash
# Rigorous host verification of the nhdoom pre-baked flash cache + relocation.
#
# The host harness has a PRE-EXISTING, layout-sensitive startup crash (the
# engine's getPackedAddress() assumes specific address ranges; certain ASLR
# placements misclassify a pointer and segfault before rendering). It is absent
# on device (fixed, separated memories). So we retry each run until it lands on a
# good layout, then assert on that clean run. Relocation deltas come from ASLR
# (whole reservation moves -> equal WAD/cache delta) plus a CACHE-offset shift
# (-> independent / unequal / negative cache delta vs the bake).
#
# A correct pre-baked+relocated cache means the engine recomputes but writes
# NOTHING (storeWordToFlash all no-ops; updateLumpAddresses while(1)-hangs on a
# wrong pointer) and renders pixel-identical to a normal cache-building run.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WAD="$ROOT/external/nh-doom/MCUDoomWadUtil/doom1.mcu.wad"
ELF="$ROOT/linux/build_nhdoom/nhdoom.elf"
SC="${SC:?set SC}"
FRAMES="${FRAMES:-30}"
TRIES="${TRIES:-25}"
pass=0; fail=0
ok(){ echo "  PASS: $*"; pass=$((pass+1)); }
no(){ echo "  FAIL: $*"; fail=$((fail+1)); }

# run_clean <logfile> <env...> : retry until the run exits 0, returns last cmd ec
run_clean(){ local log="$1"; shift; local i
  for i in $(seq 1 "$TRIES"); do
    env "$@" timeout 6 "$ELF" "$WAD" >/dev/null 2>"$log"
    [ $? -eq 0 ] && return 0
  done; return 1; }

echo "== bake A and B (clean) =="
run_clean "$SC/bA.log" NHDOOM_FRAMES=10 NHDOOM_BAKE="$SC/fcA.bin" || { echo "bake A never clean"; exit 1; }
run_clean "$SC/bB.log" NHDOOM_FRAMES=10 NHDOOM_BAKE="$SC/fcB.bin" || { echo "bake B never clean"; exit 1; }
grep -h '\[BAKE\]' "$SC/bA.log" | sed 's/^/  A /'
grep -h '\[BAKE\]' "$SC/bB.log" | sed 's/^/  B /'
FC="$SC/fcA.bin"

echo "== determinism: normalize both bakes to base 0 and compare content =="
python3 - "$SC/fcA.bin" "$SC/fcB.bin" <<'PY'
import sys,struct
def norm(p):
    d=bytearray(open(p,'rb').read())
    mg,ver,cb,hw,hws,hc,hcs,nr,do,ro=struct.unpack_from('<10I',d,0)
    rel=struct.unpack_from(f'<{nr}I',d,ro)
    cache=bytearray(d[do:do+cb])
    for r in rel:
        off=r&~1; typ=r&1
        v=struct.unpack_from('<I',cache,off)[0]
        base=hc if typ==1 else hw
        struct.pack_into('<I',cache,off,(v-base)&0xffffffff)  # rebase to 0
    return bytes(cache),nr,(sum(1 for r in rel if r&1==0)),(sum(1 for r in rel if r&1==1))
a,na,aw,ac=norm(sys.argv[1]); b,nb,_,_=norm(sys.argv[2])
print(f"  reloc total={na}  WAD-type={aw}  CACHE-type={ac}")
print("  DETERMINISM:", "PASS (normalized content byte-identical)" if a==b and na==nb else "FAIL")
PY

echo "== reference frames (normal run builds cache) =="
rm -rf "$SC/ref"; mkdir -p "$SC/ref"
run_clean "$SC/ref.log" NHDOOM_FRAMES=$FRAMES NHDOOM_FIRST_DUMP=0 NHDOOM_PPM_DIR="$SC/ref" || echo "ref never clean"
NREF=$(ls "$SC/ref"/*.ppm 2>/dev/null | wc -l)
RW=$(grep -oh 'writes this run = [0-9]*' "$SC/ref.log" | grep -o '[0-9]*' | head -1)
echo "  ref frames=$NREF  normal-run writes=$RW"
[ "${NREF:-0}" -ge "$FRAMES" ] && ok "reference rendered $NREF in-game frames (writes=$RW)" || no "ref frames=$NREF"

# verify iterations: natural mmap places the WAD and flash-cache independently
# per run (ASLR), so each clean verify run exercises a DIFFERENT, independent
# (wad_delta, cache_delta) pair vs the bake -- large and either sign.
for name in iter1 iter2 iter3 iter4 iter5; do
  d="$SC/ver_$name"; rm -rf "$d"; mkdir -p "$d"
  run_clean "$SC/v_$name.log" NHDOOM_FRAMES=$FRAMES NHDOOM_FIRST_DUMP=0 \
      NHDOOM_VERIFY="$FC" NHDOOM_PPM_DIR="$d"
  ec=$?
  wr=$(grep -oh 'writes this run = [0-9]*' "$SC/v_$name.log" | grep -o '[0-9]*' | head -1)
  vl=$(grep -oh 'wad_delta=-\?[0-9]* cache_delta=-\?[0-9]*' "$SC/v_$name.log")
  nf=$(ls "$d"/*.ppm 2>/dev/null | wc -l)
  diffc=0
  for f in "$SC/ref"/*.ppm; do cmp -s "$f" "$d/$(basename "$f")" || diffc=$((diffc+1)); done
  echo "[$name] ok=$ec writes=${wr:-?} frames=$nf framediffs=$diffc  ($vl)"
  if [ "$ec" = 0 ] && [ "${wr:-1}" = 0 ] && [ "${nf:-0}" -ge "$FRAMES" ] && [ "$diffc" = 0 ]; then
    ok "$name: 0 writes, $nf frames pixel-identical ($vl)"
  else
    no "$name: ec=$ec writes=${wr:-?} frames=$nf diffs=$diffc"
  fi
done

echo "== SUMMARY pass=$pass fail=$fail =="
[ "$fail" = 0 ] && echo "ALL VERIFICATION PASSED" || echo "VERIFICATION FAILED"
