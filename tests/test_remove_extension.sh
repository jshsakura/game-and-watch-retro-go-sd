#!/usr/bin/env bash
# remove_extension() in rg_emulators.c did memcpy(dst, path, strrchr(path,'.') - path).
# For a name with NO dot -- exactly a CPS-1 folder rom -- strrchr returns NULL and
# NULL-path underflows to a ~4 GB length, so the copy ran the length of RAM into
# peripheral space and bus-faulted the device while merely LISTING the system
# (launcher busfault CFSR=0x820, 2026-07-23). This pins the fix.
#
# It compiles the REAL function (extracted from the real rg_emulators.c, not a
# reimplementation) under AddressSanitizer and feeds it a dotless name. The fixed
# version copies the whole name; the pre-fix version's 4 GB memcpy trips ASan.
# RED-verified below against git history, same shape as the flash_alloc test.
set -u
cd "$(dirname "$0")/.."
SRC="Core/Src/retro-go/rg_emulators.c"
T=$(mktemp -d /tmp/rmext.XXXXXX)
CC="${CC:-gcc}"
rc=0

# Pull just the remove_extension function body out of a given rg_emulators.c and
# wrap it in a standalone harness. Extracts the REAL bytes, so a regression in
# the function shows here rather than in a copy that drifted.
build_one() {
    local src_text="$1" out="$2"
    {
        echo '#include <string.h>'
        echo '#include <stdio.h>'
        echo '#include <stdlib.h>'
        printf '%s\n' "$src_text"
        cat <<'EOF'
int main(void) {
    char buf[256];
    /* A CPS-1 folder rom: a directory name, no extension, no dot. */
    memset(buf, 0x7f, sizeof(buf));
    remove_extension("Warriors of Fate", buf);
    if (strcmp(buf, "Warriors of Fate") != 0) {
        printf("FAIL dotless name mangled: [%s]\n", buf);
        return 1;
    }
    /* A normal rom with an extension still loses exactly the extension. */
    remove_extension("Chrono Trigger.sfc", buf);
    if (strcmp(buf, "Chrono Trigger") != 0) {
        printf("FAIL extension not stripped: [%s]\n", buf);
        return 1;
    }
    printf("OK dotless name kept whole; extension still stripped\n");
    return 0;
}
EOF
    } > "$out"
}

extract() {   # print the remove_extension function from stdin's file
    awk '/static void remove_extension\(/{f=1} f{print} f&&/^\}/{exit}'
}

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"

echo "=== remove_extension: a dotless name (CPS-1 folder) must not 4GB-memcpy ==="
fn_now=$(extract < "$SRC")
if [ -z "$fn_now" ]; then
    echo "FAIL could not find remove_extension() in $SRC"; exit 1
fi
build_one "$fn_now" "$T/now.c"
if $CC $SAN "$T/now.c" -o "$T/now" 2>"$T/now.log" && "$T/now"; then
    :
else
    echo "FAIL current remove_extension() crashes or mangles the dotless name"
    sed -n '1,20p' "$T/now.log"
    rc=1
fi

# RED check: the same test over the pre-fix function must FAIL, or this test
# cannot see the bug it exists for. A shallow clone with no history skips it
# loudly rather than failing the build (a net that breaks CI teaches people to
# ignore CI).
PREFIX_REV=07abf423        # testbed tip before this fix; the buggy version
if git cat-file -e "$PREFIX_REV:$SRC" 2>/dev/null; then
    fn_old=$(git show "$PREFIX_REV:$SRC" | extract)
    build_one "$fn_old" "$T/old.c"
    $CC $SAN "$T/old.c" -o "$T/old" 2>/dev/null
    if ( "$T/old" >/dev/null 2>&1 ); then
        echo "FAIL the pre-fix remove_extension() passed - this test cannot see the bug"
        rc=1
    else
        echo "OK  the pre-fix version trips on the dotless name, as the device did"
    fi
else
    echo "SKIP no $PREFIX_REV in this clone (shallow?) - RED check not run"
fi

rm -rf "$T"
exit $rc
