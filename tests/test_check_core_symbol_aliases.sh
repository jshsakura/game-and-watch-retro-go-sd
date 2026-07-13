#!/usr/bin/env bash
# Pins two behaviours of scripts/check_core_symbol_aliases.py, the check that
# runs on every link and fails the build if a core reaches a symbol that only
# another core's overlay defines (see that script's own header for why that
# matters: same-address overlays alias silently instead of erroring).
#
# The check itself has no toolchain dependency in its logic — it only shells
# out to nm/objdump. So the fixtures here are synthesised nm/objdump OUTPUT
# TEXT, not real compiled objects: two tiny Python stand-ins (toolnm,
# toolobjdump) that just echo back canned text keyed by the path they were
# asked about. This keeps the test runnable in the host-tests CI job, which
# deliberately has no cross toolchain ("Cheap, no toolchain: plain gcc" —
# see .github/workflows/package.yml) and is the same job whose lack of a
# toolchain caused failure (a) below in the first place.
#
# (a) is the regression this file exists for: the script used to hard-crash
# (FileNotFoundError, exit 1) when nm was not on PATH, which is exactly the
# SD_CARD=0 CI job's situation (toolchain present but not on a non-login
# PATH). It must now warn on stderr and exit 0.
#
# (b)/(c) pin that the fix did not also water down the real check: a genuine
# cross-overlay alias must still fail, and a clean tree must still pass.
set -u
cd "$(dirname "$0")/.."
CHECKER="${CHECKER:-scripts/check_core_symbol_aliases.py}"
PY="$(command -v python3)"
T="$(mktemp -d /tmp/sca_test.XXXXXX)"
trap 'rm -rf "$T"' EXIT
rc=0

# --- fake nm/objdump: read canned text instead of touching a real binary ---
# Object files and the "elf" are plain fixture files/dirs holding the exact
# text these tools would print; toolnm/toolobjdump just relay it. Naming
# matters: check_core_symbol_aliases.py derives objdump as
# NM.replace("nm", "objdump"), so the fake nm's path must contain "nm"
# exactly where substituting "objdump" yields the fake objdump's path.
mkdir -p "$T/tools"
cat > "$T/tools/toolnm" <<'PYEOF'
#!/usr/bin/env python3
import sys, os
path = sys.argv[-1]
if os.path.isdir(path):
    path = os.path.join(path, "nm.txt")
sys.stdout.write(open(path).read() if os.path.isfile(path) else "")
PYEOF
cat > "$T/tools/toolobjdump" <<'PYEOF'
#!/usr/bin/env python3
import sys, os
args = sys.argv[1:]
elf = args[-1]
section = next((a.split("=", 1)[1] for a in args if a.startswith("--section=")), "")
core = section.replace(".overlay_", "")
p = os.path.join(elf, f"overlay_{core}.txt")
sys.stdout.write(open(p).read() if os.path.isfile(p) else "")
PYEOF
chmod +x "$T/tools/toolnm" "$T/tools/toolobjdump"
FAKE_NM="$T/tools/toolnm"

echo "=== check_core_symbol_aliases: nm missing must warn+skip, not fail the build ==="
# No nm/llvm-nm reachable, NM unset — the exact SD_CARD=0-job situation.
mkdir -p /tmp/sca_emptybin_$$ && EMPTYBIN="/tmp/sca_emptybin_$$"
env -i PATH="$EMPTYBIN" "$PY" "$CHECKER" /nonexistent-build /nonexistent.elf \
    >"$T/a.out" 2>"$T/a.err"
a_rc=$?
rmdir "$EMPTYBIN"
if [ "$a_rc" -eq 0 ] && grep -q "no nm on PATH" "$T/a.err"; then
    echo "OK  exits 0 and warns on stderr when nm is unavailable"
else
    echo "FAIL missing-nm case: exit=$a_rc stderr=[$(cat "$T/a.err")]"
    rc=1
fi

echo "=== check_core_symbol_aliases: a real cross-overlay alias still fails the build ==="
mkdir -p "$T/build_alias/coreA" "$T/build_alias/coreB" "$T/image_alias.elf"
# coreA references owned_by_coreB (U); only coreB defines it (T). coreB's
# overlay disassembly (.overlay_coreA) actually branches to owned_by_coreB's
# address — a LIVE reference, which is what the script requires before it
# calls this an alias rather than dead code the linker would drop.
cat > "$T/build_alias/coreA/coreA.o" <<'EOF'
00000000 T coreA_entry
         U owned_by_coreB
EOF
cat > "$T/build_alias/coreB/coreB.o" <<'EOF'
00000010 T owned_by_coreB
EOF
cat > "$T/image_alias.elf/nm.txt" <<'EOF'
24000000 T coreA_entry
24000010 T owned_by_coreB
EOF
cat > "$T/image_alias.elf/overlay_coreA.txt" <<'EOF'
24000000 <coreA_entry>:
24000004:	bl	24000010 <owned_by_coreB>
EOF
: > "$T/image_alias.elf/overlay_coreB.txt"
NM="$FAKE_NM" "$PY" "$CHECKER" "$T/build_alias" "$T/image_alias.elf" >"$T/b.out" 2>"$T/b.err"
b_rc=$?
if [ "$b_rc" -ne 0 ] && grep -q "coreA" "$T/b.out" && grep -q "owned_by_coreB" "$T/b.out"; then
    echo "OK  exits non-zero on a genuine cross-overlay alias"
else
    echo "FAIL alias case: expected non-zero exit naming coreA/owned_by_coreB, got exit=$b_rc:"
    cat "$T/b.out"
    rc=1
fi

echo "=== check_core_symbol_aliases: a clean tree passes ==="
mkdir -p "$T/build_clean/coreC" "$T/build_clean/coreD" "$T/image_clean.elf"
cat > "$T/build_clean/coreC/coreC.o" <<'EOF'
00000000 T coreC_entry
EOF
cat > "$T/build_clean/coreD/coreD.o" <<'EOF'
00000000 T coreD_entry
EOF
cat > "$T/image_clean.elf/nm.txt" <<'EOF'
24000000 T coreC_entry
24001000 T coreD_entry
EOF
cat > "$T/image_clean.elf/overlay_coreC.txt" <<'EOF'
24000000 <coreC_entry>:
24000004:	bx lr
EOF
cat > "$T/image_clean.elf/overlay_coreD.txt" <<'EOF'
24001000 <coreD_entry>:
24001004:	bx lr
EOF
NM="$FAKE_NM" "$PY" "$CHECKER" "$T/build_clean" "$T/image_clean.elf" >"$T/c.out" 2>"$T/c.err"
c_rc=$?
if [ "$c_rc" -eq 0 ] && grep -q "^OK" "$T/c.out"; then
    echo "OK  exits 0 on a clean tree"
else
    echo "FAIL clean case: expected exit 0 with an OK line, got exit=$c_rc:"
    cat "$T/c.out"
    rc=1
fi

exit $rc
