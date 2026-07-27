#!/usr/bin/env bash
# Pins scripts/check_resident_init_array.py, the gate that stops a boot-time C++
# constructor from pointing into overlay RAM.
#
# Why it exists: startup calls __libc_init_array() before main(), which calls
# every pointer in the RESIDENT .init_array. A pointer aimed at RAM_EMU is a call
# into a region holding no core yet, so the device dies before the LCD is
# initialised -- black screen, no BSOD, no boot-rescue screen, and the patched OFW
# still boots, which makes it look like anything except a firmware fault. It
# shipped in testbed-full-20260727-1501 and -1610 (one constructor,
# _GLOBAL__sub_I_onTap from tamapoke_ui.cpp), and in C64/Frodo before that.
#
# Fixtures are synthesised objdump/nm output, not real ELFs: the script only
# shells out to those two tools, so a stand-in that replays canned text keeps
# this runnable in the host-tests CI job, which has no cross toolchain.
set -u
cd "$(dirname "$0")/.."
CHECKER="${CHECKER:-scripts/check_resident_init_array.py}"
PY="$(command -v python3)"
T="$(mktemp -d /tmp/initarray_test.XXXXXX)"
trap 'rm -rf "$T"' EXIT
rc=0

# --- fake objdump/nm ------------------------------------------------------
# The "elf" is a directory: <elf>/h.txt answers `objdump -h`, <elf>/<section>.txt
# answers `objdump -s -j <section>`, and <elf>/nm.txt answers `nm -n`. A section
# with no fixture file exits 1, which is what real objdump does for a section
# that is not in the binary -- the case that broke the first draft of the gate.
# The stand-in must be named so that replacing "objdump" with "nm" in its path
# yields the nm stand-in, because the script derives nm that way.
mkdir -p "$T/tools"
cat > "$T/tools/toolobjdump" <<'PYEOF'
#!/usr/bin/env python3
import sys, os
args = sys.argv[1:]
elf = args[-1]
if "-h" in args:
    sys.exit(0 if os.path.isdir(elf) else 1)
section = args[args.index("-j") + 1] if "-j" in args else ""
p = os.path.join(elf, section + ".txt")
if not os.path.isfile(p):
    sys.exit(1)          # section absent, exactly like real objdump
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

# A .init_array holding two words: 0x08101109 (flash, fine) and 0x2402dc09
# (RAM_EMU + Thumb bit) -- the pointer that shipped.
mk_bad() {
    mkdir -p "$1"
    cat > "$1/.init_array.txt" <<'EOF'
Contents of section .init_array:
 813a93c 09111008 09dc0224                    .......$
EOF
    cat > "$1/nm.txt" <<'EOF'
2402dc08 t _GLOBAL__sub_I_onTap
EOF
}

echo "=== check_resident_init_array: a ctor in overlay RAM must FAIL, and be named ==="
mk_bad "$T/bad.elf"
"$PY" "$CHECKER" "$OBJDUMP" "$T/bad.elf" >"$T/a.out" 2>&1
a_rc=$?
if [ "$a_rc" -ne 0 ] \
   && grep -q "FAILED" "$T/a.out" \
   && grep -q "0x2402dc08" "$T/a.out" \
   && grep -q "_GLOBAL__sub_I_onTap" "$T/a.out"; then
    echo "OK  exits non-zero, prints the address and the symbol"
else
    echo "FAIL overlay-ctor case: expected non-zero exit naming 0x2402dc08, got exit=$a_rc:"
    cat "$T/a.out"
    rc=1
fi

echo "=== check_resident_init_array: a missing .preinit_array must not turn into a SKIP ==="
# THE REGRESSION IN THE GATE ITSELF. objdump exits non-zero for a section that is
# not in the binary, and the first draft read that as "tool missing" and skipped
# -- reporting SKIPPED on the exact ELF it was written to catch. The bad fixture
# above deliberately has no .preinit_array, so if that confusion ever comes back
# this case fails: a real fault must not be reported as "not verified".
if ! grep -q "SKIPPED" "$T/a.out"; then
    echo "OK  an absent section is 'nothing to check', not 'could not check'"
else
    echo "FAIL the gate skipped instead of checking:"
    cat "$T/a.out"
    rc=1
fi

echo "=== check_resident_init_array: flash-only ctors pass ==="
mkdir -p "$T/good.elf"
cat > "$T/good.elf/.init_array.txt" <<'EOF'
Contents of section .init_array:
 813f2d4 09111008                             ....
EOF
: > "$T/good.elf/nm.txt"
"$PY" "$CHECKER" "$OBJDUMP" "$T/good.elf" >"$T/b.out" 2>&1
b_rc=$?
if [ "$b_rc" -eq 0 ] && grep -q "OK" "$T/b.out" && ! grep -q "SKIPPED" "$T/b.out"; then
    echo "OK  exits 0 with every ctor in internal flash"
else
    echo "FAIL flash-only case: expected exit 0 with an OK line, got exit=$b_rc:"
    cat "$T/b.out"
    rc=1
fi

echo "=== check_resident_init_array: an unusable objdump must skip loudly, not fail ==="
"$PY" "$CHECKER" "$T/tools/nonexistent-objdump" "$T/good.elf" >"$T/c.out" 2>&1
c_rc=$?
if [ "$c_rc" -eq 0 ] && grep -q "SKIPPED" "$T/c.out"; then
    echo "OK  exits 0 and says SKIPPED when objdump cannot be run"
else
    echo "FAIL missing-tool case: expected exit 0 with a SKIPPED line, got exit=$c_rc:"
    cat "$T/c.out"
    rc=1
fi

echo "=== every C++ overlay captures its own .init_array in both link scripts ==="
# The gate above catches the leak in a build that HAS the core compiled. This
# catches the omission by inspection, in both scripts, so a core added while its
# objects are not being built still cannot get it wrong.
for core in tgb a2600 lynx c64 tamapoke; do
    for ld in STM32H7B0VBTx_SDCARD.ld STM32H7B0VBTx_FLASH.ld; do
        if grep -q "__init_array_${core}_start__" "$ld" \
           && grep -q "KEEP(build/[a-z0-9_]*/\*\.o(\.init_array\*))" "$ld"; then
            :
        else
            echo "FAIL $ld does not capture .init_array for '$core'"
            rc=1
        fi
    done
done
[ "$rc" -eq 0 ] && echo "OK  tgb/a2600/lynx/c64/tamapoke all captured in both scripts"

exit $rc
