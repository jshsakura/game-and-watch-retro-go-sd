#!/bin/bash
# Normalize MCD sector 0 of repro-rip images so the BIOS disc checks pass:
#  - system ID at IP.BIN 0x00 must be one of SEGADISC/SEGABOOTDISC/SEGADATADISC/
#    SEGADISCSYSTEM (LUNAR2 repro shipped "LUNAR2     ")
#  - security string+code block must sit at 0x200-0x783 (BIOS checks 0x669);
#    repros ship it at arbitrary offsets with zeros at 0x600-0x7FF
# Usage: segacd_fix_security.sh [game_dir]  (default: all games under gnw-segacd-cue)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TPL="$ROOT/tools/ipsec_template.bin"
BASE_DIR="${1:-/media/pi/EXTERNAL/gnw-segacd-cue}"
[ -f "$TPL" ] || { echo "template missing: $TPL" >&2; exit 1; }

patch_dir() {
    local dir="$1" t1
    t1=$(ls "$dir" 2>/dev/null | grep -E '\(Track 0?1\)\.bin$' | head -1)
    [ -n "$t1" ] && t1="$dir/$t1"
    [ -n "$t1" ] || { echo "SKIP (no track01): $dir"; return; }
    python3 - "$t1" "$TPL" <<'EOF'
import sys
path, tpl = sys.argv[1], sys.argv[2]
blk = open(tpl, 'rb').read()
assert len(blk) == 0x584, len(blk)
ok_ids = (b"SEGADISC", b"SEGABOOTDISC", b"SEGADATADISC", b"SEGADISCSYSTEM")
with open(path, 'r+b') as f:
    f.seek(16)
    sysid = f.read(16)
    f.seek(16 + 0x669)
    sec = f.read(8)
    need_id = not sysid.startswith(ok_ids)
    need_sec = sec != b"PRODUCED"
    if not need_id and not need_sec:
        print(f"SKIP (already ok): {path}")
        sys.exit(0)
    import os, shutil
    if not os.path.exists(path + ".orig"):
        shutil.copy2(path, path + ".orig")
    if need_id:
        f.seek(16)
        f.write(b"SEGADISCSYSTEM  ")
        print(f"patched sysid {sysid!r} -> SEGADISCSYSTEM")
    if need_sec:
        f.seek(16 + 0x200)
        f.write(blk)
        print("patched sec0[0x200..0x784)")
EOF
}

if [ -d "$BASE_DIR" ] && ls "$BASE_DIR"/*Track\ 01*.bin >/dev/null 2>&1; then
    patch_dir "$BASE_DIR"
else
    for d in "$BASE_DIR"/*/; do
        [ -d "$d" ] && patch_dir "${d%/}"
    done
fi
echo "DONE"
