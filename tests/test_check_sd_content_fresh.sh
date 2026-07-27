#!/usr/bin/env bash
# Pins scripts/check_sd_content_fresh.sh, the gate that stops a release tar
# carrying a previous build's output.
#
# `release` rolls the tar from sd_content/ wholesale, so whatever is in that
# directory ships regardless of what this build produced. It has been wrong
# twice, both on 0727: 305 CC BY-NC sprite files staged for testing, and then
# cores/32x.bin + cores/32x.xip (~1 MB) left over from before the 32X core was
# deleted -- a core the firmware can no longer even dispatch. Every audit at the
# time asked git, which cannot see either.
#
# Cases: a stale file must block packaging and name itself; a clean tree must
# pass; repo content that is copied rather than generated (bios blobs) must NOT
# be flagged just for being older; and a missing ELF must skip loudly rather
# than fail, because a safety net that breaks builds gets ignored (CLAUDE.md).
set -u
cd "$(dirname "$0")/.."
CHECKER="${CHECKER:-scripts/check_sd_content_fresh.sh}"
T="$(mktemp -d /tmp/sdfresh_test.XXXXXX)"
trap 'rm -rf "$T"' EXIT
rc=0

# The checker looks at ./sd_content, so each case runs in its own sandbox dir
# with a fake tree. CHECKER is resolved to an absolute path first.
CHECKER_ABS="$(cd "$(dirname "$CHECKER")" && pwd)/$(basename "$CHECKER")"

mk_tree() {
    # $1 = sandbox dir. ELF is stamped between the old and new files so that
    # "older than the ELF" is unambiguous without relying on sleep resolution.
    local d="$1"
    mkdir -p "$d/sd_content/cores" "$d/sd_content/roms/homebrew" \
             "$d/sd_content/bios/msx" "$d/build"
    : > "$d/sd_content/cores/stale_removed_core.bin"
    : > "$d/sd_content/bios/msx/MSX.rom"
    touch -d '2020-01-01 00:00:00' "$d/sd_content/cores/stale_removed_core.bin" \
                                   "$d/sd_content/bios/msx/MSX.rom"
    : > "$d/build/gw_retro_go.elf"
    touch -d '2020-06-01 00:00:00' "$d/build/gw_retro_go.elf"
    : > "$d/sd_content/cores/fresh.bin"
    : > "$d/sd_content/roms/homebrew/Fresh.bin"
    touch -d '2020-12-01 00:00:00' "$d/sd_content/cores/fresh.bin" \
                                    "$d/sd_content/roms/homebrew/Fresh.bin"
}

echo "=== check_sd_content_fresh: a stale build output must block packaging ==="
mk_tree "$T/stale"
( cd "$T/stale" && bash "$CHECKER_ABS" build/gw_retro_go.elf ) >"$T/a.out" 2>&1
a_rc=$?
if [ "$a_rc" -ne 0 ] \
   && grep -q "FAILED" "$T/a.out" \
   && grep -q "cores/stale_removed_core.bin" "$T/a.out" \
   && grep -q "rm sd_content/cores/stale_removed_core.bin" "$T/a.out"; then
    echo "OK  exits non-zero, names the file, and prints the rm that fixes it"
else
    echo "FAIL stale case: expected non-zero exit naming the stale file, got exit=$a_rc:"
    cat "$T/a.out"
    rc=1
fi

echo "=== check_sd_content_fresh: copied repo content (bios blobs) must not be flagged ==="
# MSX.rom is older than the ELF on purpose. It is repo content the build copies,
# not something create_sd_data writes, so flagging it would make the gate cry
# wolf on every release -- which is how a gate gets disabled.
if ! grep -q "MSX.rom" "$T/a.out"; then
    echo "OK  bios/msx/MSX.rom not reported despite being older than the ELF"
else
    echo "FAIL bios blob was flagged; the check must be scoped to build outputs:"
    cat "$T/a.out"
    rc=1
fi

echo "=== check_sd_content_fresh: a clean tree passes ==="
mk_tree "$T/clean"
rm -f "$T/clean/sd_content/cores/stale_removed_core.bin"
( cd "$T/clean" && bash "$CHECKER_ABS" build/gw_retro_go.elf ) >"$T/b.out" 2>&1
b_rc=$?
if [ "$b_rc" -eq 0 ] && grep -q "OK" "$T/b.out"; then
    echo "OK  exits 0 when every build output is newer than the ELF"
else
    echo "FAIL clean case: expected exit 0, got exit=$b_rc:"
    cat "$T/b.out"
    rc=1
fi

echo "=== check_sd_content_fresh: a missing ELF must skip loudly, not fail ==="
mk_tree "$T/noelf"
rm -f "$T/noelf/build/gw_retro_go.elf"
( cd "$T/noelf" && bash "$CHECKER_ABS" build/gw_retro_go.elf ) >"$T/c.out" 2>&1
c_rc=$?
if [ "$c_rc" -eq 0 ] && grep -q "SKIPPED" "$T/c.out"; then
    echo "OK  exits 0 and says SKIPPED with no ELF to compare against"
else
    echo "FAIL missing-ELF case: expected exit 0 with a SKIPPED line, got exit=$c_rc:"
    cat "$T/c.out"
    rc=1
fi

exit $rc
