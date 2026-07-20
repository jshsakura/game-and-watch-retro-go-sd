#!/bin/bash
# Regenerates Core/Src/porting/cps1/cps1_rc_generated.c from the canonical
# test program (the same bytes cps1_core.c's s_cpu_test_program hard-codes)
# and diffs it against the checked-in copy. A generated file that can drift
# from its generator silently is the class of bug this catches -- fails
# loudly instead if translate.py and cps1_rc_generated.c ever disagree.
#
#   bash tools/cps1_recompiler/selftest.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Keep in lockstep with s_cpu_test_program in Core/Src/porting/cps1/cps1_core.c.
printf '\x70\x0A\x72\x00\x56\x81\x51\xC8\xFF\xFC\x4E\x75' > "$TMP/test_program.bin"

python3 tools/cps1_recompiler/translate.py "$TMP/test_program.bin" 0 \
    --out "$TMP/cps1_rc_generated.c"

if ! diff -u Core/Src/porting/cps1/cps1_rc_generated.c "$TMP/cps1_rc_generated.c"; then
    echo "[cps1-recomp] FAIL: checked-in cps1_rc_generated.c is stale -- regenerate it" >&2
    exit 1
fi

echo "[cps1-recomp] OK: cps1_rc_generated.c matches translate.py's output"

# Build test: compile the generated recompiler against the same interpreter
# runtime it depends on, and cross-check its register state against the
# interpreter's for the identical program.
CPS1=Core/Src/porting/cps1
cat > "$TMP/verify_rc.c" <<'EOF'
#include <stdio.h>
#include "cps1_cpu68k.h"
void cps1_rc_translated(cps1_cpu68k_t *regs);
int main(void) {
    cps1_cpu68k_t interp, rc;
    static const unsigned char prog[] = {
        0x70, 0x0A, 0x72, 0x00, 0x56, 0x81, 0x51, 0xC8, 0xFF, 0xFC, 0x4E, 0x75,
    };
    cps1_cpu68k_reset(&interp, prog, sizeof(prog));
    cps1_cpu68k_run(&interp, 64);
    cps1_cpu68k_reset(&rc, prog, sizeof(prog));
    cps1_rc_translated(&rc);
    if (interp.d[0] != rc.d[0] || interp.d[1] != rc.d[1] || interp.halted != rc.halted) {
        fprintf(stderr, "MISMATCH: interp D0=%08x D1=%08x halted=%d | rc D0=%08x D1=%08x halted=%d\n",
                interp.d[0], interp.d[1], interp.halted, rc.d[0], rc.d[1], rc.halted);
        return 1;
    }
    printf("OK: interpreter and recompiler agree, D0=%08x D1=%08x\n", rc.d[0], rc.d[1]);
    return 0;
}
EOF

gcc -Wall -I "$CPS1" -o "$TMP/verify_rc" "$TMP/verify_rc.c" \
    "$CPS1/cps1_cpu68k.c" "$CPS1/cps1_rc_runtime.c" "$CPS1/cps1_rc_generated.c"
"$TMP/verify_rc"
