#!/bin/bash
# Nothing called patches/apply.sh, so `make release` built the picodrive 32X core
# without the SH-2 loop patch and printed nothing about it. Pin the wiring: the
# Makefile must run apply.sh before the 32X objects, and apply.sh must fail
# loudly (non-zero) when the picodrive patch neither applies nor is applied.
set -u
rc=0
ok()   { echo "  OK   $1"; }
bad()  { echo "  FAIL $1"; rc=1; }

grep -q 'bash patches/apply.sh' Makefile.common && ok "ApplyPatches runs patches/apply.sh" || bad "no make step runs patches/apply.sh"
grep -q '^\$(MD32X_OBJECTS): | ApplyPatches' Makefile.common && ok "32X objects wait for ApplyPatches" || bad "32X objects do not depend on ApplyPatches"
grep -q 'external/picodrive' Makefile.common && grep -q 'case "\$\$sm_path"' Makefile.common && ok "dirty-submodule check exempts the patched submodules" || bad "dirty check would reject the patched submodule"

# Behavior: a scratch repo where external/picodrive exists but the patch matches nothing.
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/patches" "$tmp/external/picodrive" "$tmp/external/gwenesis"
cp patches/apply.sh "$tmp/patches/"
cp patches/picodrive-32x-sh2-loop.patch "$tmp/patches/"
for d in picodrive gwenesis; do git -C "$tmp/external/$d" init -q; done
if bash "$tmp/patches/apply.sh" >/dev/null 2>"$tmp/err"; then
    bad "apply.sh exited 0 although the picodrive patch cannot apply"
else
    grep -q "neither applies nor is already applied" "$tmp/err" && ok "apply.sh fails loudly on an unappliable picodrive patch" || bad "apply.sh failed without the explanation"
fi
exit $rc
