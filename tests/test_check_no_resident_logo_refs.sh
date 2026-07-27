#!/usr/bin/env bash
# Pins scripts/check_no_resident_logo_refs.py, the gate that stops resident code
# from holding the address of a .sdcard_logo symbol.
#
# .sdcard_logo is staging, not memory: objcopy hands its contents to
# /bios/logo.bin, and its symbol addresses are extflash LOAD addresses
# (0x004xxxxx) that nothing can read at runtime. Logos are reached by INDEX
# through rg_get_logo(). One line broke that rule --
#
#     case RG_LOGO_HEADER_FAVORITES:
#         return (retro_logo_image *)&header_favorites;   /* LOGO_DATA */
#
# -- with a comment claiming the opposite ("served from flash"), and entering the
# favorites tab faulted on the device at 0x004c1d5e, which is header_favorites
# exactly. CPS-1's wordmark had the identical bug earlier.
#
# Fixtures replay objdump/nm text, so no cross toolchain is needed (the
# host-tests CI job has none).
set -u
cd "$(dirname "$0")/.."
CHECKER="${CHECKER:-scripts/check_no_resident_logo_refs.py}"
PY="$(command -v python3)"
T="$(mktemp -d /tmp/logoref_test.XXXXXX)"
trap 'rm -rf "$T"' EXIT
rc=0

# --- fake objdump/nm ------------------------------------------------------
# "elf" is a directory: h.txt answers `objdump -h`, <section>.txt answers
# `objdump -s -j <section>`, nm.txt answers `nm -n`. A section with no fixture
# exits 1, like real objdump for an absent section.
mkdir -p "$T/tools"
cat > "$T/tools/toolobjdump" <<'PYEOF'
#!/usr/bin/env python3
import sys, os
args = sys.argv[1:]
elf = args[-1]
if "-h" in args:
    p = os.path.join(elf, "h.txt")
    if not os.path.isfile(p):
        sys.exit(1)
    sys.stdout.write(open(p).read()); sys.exit(0)
section = args[args.index("-j") + 1] if "-j" in args else ""
p = os.path.join(elf, section + ".txt")
if not os.path.isfile(p):
    sys.exit(1)
sys.stdout.write(open(p).read())
PYEOF
cat > "$T/tools/toolnm" <<'PYEOF'
#!/usr/bin/env python3
import sys, os
p = os.path.join(sys.argv[-1], "nm.txt")
sys.stdout.write(open(p).read() if os.path.isfile(p) else "")
PYEOF
chmod +x "$T/tools/toolobjdump" "$T/tools/toolnm"
OBJDUMP="$T/tools/toolobjdump"
NM="$T/tools/toolnm"

mk_elf() {
    # $1 dir, $2 = the word to bake into .text (as 8 hex chars, little-endian)
    mkdir -p "$1"
    cat > "$1/h.txt" <<'EOF'
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         00000010  08119b40  08119b40  00001000  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, CODE
  1 .sdcard_logo  00000100  004c1d00  004c1d00  00002000  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, DATA
EOF
    # 16 bytes; the ASCII column is included on purpose -- it is what made an
    # earlier draft of the parser crash and then misread data.
    cat > "$1/.text.txt" <<EOF
Contents of section .text:
 8119b40 00000000 $2 00000000 00000000  ....^..\$....abcd
EOF
    cat > "$1/nm.txt" <<'EOF'
004c1d5e R header_favorites
0813d8b8 T logo_rgo
EOF
}

echo "=== check_no_resident_logo_refs: a staged-logo address in flash must FAIL ==="
# 0x004c1d5e little-endian = 5e 1d 4c 00
mk_elf "$T/bad.elf" "5e1d4c00"
"$PY" "$CHECKER" "$OBJDUMP" "$NM" "$T/bad.elf" >"$T/a.out" 2>&1
a_rc=$?
if [ "$a_rc" -ne 0 ] \
   && grep -q "FAILED" "$T/a.out" \
   && grep -q "0x004c1d5e" "$T/a.out" \
   && grep -q "header_favorites" "$T/a.out"; then
    echo "OK  exits non-zero, names the flash location, the word and the symbol"
else
    echo "FAIL baked-address case: expected non-zero exit naming header_favorites, got exit=$a_rc:"
    cat "$T/a.out"
    rc=1
fi

echo "=== check_no_resident_logo_refs: the ASCII column must not be read as data ==="
# The fixture's ASCII column ends in "abcd", which is valid hex. If the parser
# takes it for a data word it reads past the section and can invent findings --
# or crash, which is what the first draft did. Nothing may be reported beyond
# the one real hit above.
if [ "$(grep -c "flash 0x" "$T/a.out")" = "1" ]; then
    echo "OK  exactly one finding; the ASCII column was not parsed as data"
else
    echo "FAIL parser read outside the section:"
    cat "$T/a.out"
    rc=1
fi

echo "=== check_no_resident_logo_refs: a clean build passes ==="
mk_elf "$T/good.elf" "deadbeef"
"$PY" "$CHECKER" "$OBJDUMP" "$NM" "$T/good.elf" >"$T/b.out" 2>&1
b_rc=$?
if [ "$b_rc" -eq 0 ] && grep -q "OK" "$T/b.out" && ! grep -q "SKIPPED" "$T/b.out"; then
    echo "OK  exits 0 when no staged address is baked into flash"
else
    echo "FAIL clean case: expected exit 0 with an OK line, got exit=$b_rc:"
    cat "$T/b.out"
    rc=1
fi

echo "=== check_no_resident_logo_refs: an unusable toolchain must skip loudly ==="
"$PY" "$CHECKER" "$OBJDUMP" "$T/tools/nonexistent-nm" "$T/good.elf" >"$T/c.out" 2>&1
c_rc=$?
if [ "$c_rc" -eq 0 ] && grep -q "SKIPPED" "$T/c.out"; then
    echo "OK  exits 0 and says SKIPPED rather than failing the build"
else
    echo "FAIL missing-tool case: expected exit 0 with a SKIPPED line, got exit=$c_rc:"
    cat "$T/c.out"
    rc=1
fi

echo "=== rg_get_logo returns only INT_LOGO_DATA art from its early switch ==="
# By inspection, in the real source: the switch that answers before the
# index/cache path may only hand back logos declared INT_LOGO_DATA (resident in
# internal flash). This is the shape of the bug, stated directly.
src="Core/Src/retro-go/rg_logos.c"
int_syms=$(grep -oE "const retro_logo_image \w+ INT_LOGO_DATA" "$src" | awk '{print $3}' | tr '\n' ' ')
# Strip comments before reading the code: the fix for this very bug left an
# explanatory comment naming &header_favorites, and a check that cannot tell a
# comment from a statement would fail on the sentence describing the repair.
switch_body=$(sed -n '/Logos always included in internal flash/,/static_assert(INT_LOGO_COUNT/p' "$src" \
              | sed -e 's://.*::' -e 's:/\*.*\*/::' | grep -v '^\s*\*' | grep -v '^\s*/\*')
bad_returns=""
while IFS= read -r sym; do
    [ -n "$sym" ] || continue
    case " $int_syms " in
        *" $sym "*) ;;
        *) bad_returns="$bad_returns $sym" ;;
    esac
done <<EOF
$(printf '%s\n' "$switch_body" | grep -oE "&\w+" | tr -d '&')
EOF
if [ -z "$bad_returns" ]; then
    echo "OK  early switch returns only INT_LOGO_DATA logos"
else
    echo "FAIL early switch returns non-resident art:$bad_returns"
    echo "     Those live in .sdcard_logo; their addresses are unreadable at runtime."
    rc=1
fi

exit $rc
