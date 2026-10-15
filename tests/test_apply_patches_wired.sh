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

# The md-defer patch is written against the sh2-loop result, so apply.sh must name
# both and apply them in that order, and both files must exist.
[ -f patches/picodrive-32x-md-defer.patch ] && ok "md-defer patch file exists" || bad "patches/picodrive-32x-md-defer.patch is missing"
loop_line=$(grep -n 'for pd in picodrive-32x-sh2-loop picodrive-32x-md-defer' patches/apply.sh | cut -d: -f1)
[ -n "$loop_line" ] && ok "apply.sh applies sh2-loop before md-defer" || bad "apply.sh does not apply both picodrive patches in order"

# These patches are written against CRLF sources. Without `-text` git normalises
# the patch to LF on commit and every hunk then fails to apply, which only shows
# up in the build, so pin the attribute and that the committed blob kept its CRs.
for pd in picodrive-32x-sh2-loop picodrive-32x-md-defer; do
  grep -q "^patches/$pd.patch -text" .gitattributes && ok "$pd.patch is -text (CRLF kept)" || bad "$pd.patch has no -text in .gitattributes"
  if git rev-parse --git-dir >/dev/null 2>&1 && git cat-file -e "HEAD:patches/$pd.patch" 2>/dev/null; then
    [ "$(git show "HEAD:patches/$pd.patch" | grep -c $'\r')" -gt 0 ] && ok "$pd.patch blob keeps its CRs" || bad "$pd.patch blob lost its CRs"
  fi
done

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
