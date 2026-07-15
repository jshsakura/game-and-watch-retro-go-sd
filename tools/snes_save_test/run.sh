#!/bin/bash
# SNES savestate cold-resume proof (two-process A/B).
#   bash tools/snes_save_test/run.sh            # 3-game round-trip + refusal tests
# Compiles Core/Src/porting/snes/main_snes.c ITSELF (the device serializer),
# the snes lib at the submodule's COMMITTED state (another fork has uncommitted
# DSP-1 edits in cart.c/cart.h/snes_other.c — we build what commit 661d6c7e
# links against), and the shim that scripts the run.
set -e
cd "$(dirname "$0")/../.."

O=/tmp/snes_save_test
mkdir -p "$O"
rm -f "$O"/*.o

# committed versions of the dirty submodule files
for f in cart.c cart.h snes_other.c; do
  git -C external/sm show "HEAD:src/snes/$f" > "$O/$f"
done

# -iquote (not -I) for external/sm/src: its features.h shadows glibc <features.h>
# on the angle-bracket path and detonates the entire libc header chain.
CF="-O2 -g -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w
    -Itools/snes_save_test/shim -I$O -Iexternal/sm -iquote external/sm/src -iquote external/sm/src/snes"

# snes lib: everything except the dirty two .c (compiled from the extracted copies)
for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  base="$(basename "$f")"
  [ "$base" = "cart.c" ] && continue
  [ "$base" = "snes_other.c" ] && continue
  [ "$base" = "dsp1_hle.c" ] && continue
  gcc -c $CF "$f" -o "$O/${base%.c}.o"
done
# dirty files: committed copies (force our cart.h via -I$O first)
gcc -c $CF -I"$O" "$O/cart.c" -o "$O/cart.o"
gcc -c $CF -I"$O" "$O/snes_other.c" -o "$O/snes_other.o"

# the device porting layer, compiled as-is
gcc -c $CF Core/Src/porting/snes/main_snes.c -o "$O/main_snes.o"
gcc -c $CF tools/snes_save_test/shim.c -o "$O/shim.o"
gcc -o "$O/save_test" "$O"/*.o -lm
echo "BUILD OK"

SFC="/home/ubuntu/app/jupyterLab/notebooks/game-and-what/backend/data/_collected_sfc"
SAVE=600; END=1200

run_game() {
  local name="$1" rom="$2"
  local st="$O/state_${name}.sav"
  local a b
  a=$("$O/save_test" A "$rom" "$st" $SAVE $END 2>/dev/null | grep -oE "AV=[0-9a-f]+ MACHINE=[0-9a-f]+")
  b=$("$O/save_test" B "$rom" "$st" $SAVE $END 2>/dev/null | grep -oE "AV=[0-9a-f]+ MACHINE=[0-9a-f]+")
  if [ -n "$a" ] && [ "$a" = "$b" ]; then
    echo "PASS  $name  $a"
  else
    echo "FAIL  $name  A=[$a] B=[$b]"
  fi
}

run_game zelda "$SFC/젤다의전설 신들의트라이포스 (Zelda A Link to the Past).smc"
run_game dkc   "$SFC/동키콩 (Donkey Kong Country).smc"
run_game tmnt  "$SFC/닌자거북이 (TMNT Turtles in Time).smc"

# ---- refusal paths (on the zelda state) --------------------------------------
ST="$O/state_zelda.sav"
echo "--- refusal tests ---"
# 1. corrupt magic
cp "$ST" "$O/bad_magic.sav"; printf '\xde\xad\xbe\xef' | dd of="$O/bad_magic.sav" bs=1 count=4 conv=notrunc 2>/dev/null
EXPECT_REFUSE=1 "$O/save_test" B "$SFC/젤다의전설 신들의트라이포스 (Zelda A Link to the Past).smc" "$O/bad_magic.sav" $SAVE $END 2>/dev/null | grep REFUSAL_RESULT | sed 's/^/magic-corrupt:   /'
# 2. version bump
cp "$ST" "$O/bad_ver.sav"; printf '\x63\x00\x00\x00' | dd of="$O/bad_ver.sav" bs=1 seek=4 count=4 conv=notrunc 2>/dev/null
EXPECT_REFUSE=1 "$O/save_test" B "$SFC/젤다의전설 신들의트라이포스 (Zelda A Link to the Past).smc" "$O/bad_ver.sav" $SAVE $END 2>/dev/null | grep REFUSAL_RESULT | sed 's/^/version-bump:    /'
# 3. truncated payload (keep header + half the payload)
SZ=$(stat -c%s "$ST"); head -c $(( 12 + (SZ-12)/2 )) "$ST" > "$O/truncated.sav"
EXPECT_REFUSE=1 "$O/save_test" B "$SFC/젤다의전설 신들의트라이포스 (Zelda A Link to the Past).smc" "$O/truncated.sav" $SAVE $END 2>/dev/null | grep REFUSAL_RESULT | sed 's/^/truncated:       /'

# NOTE: this cold-resume round-trip needs a ROM (boots Zelda/DKC/TMNT) and
# external/sm — it is a LOCAL proof, NOT run in CI. The CI-safe slices of the
# same code (mapper gate, savestate stamp refusal) live in tests/run.sh via
# tests/test_snes_cart_gate.c and tests/test_snes_state_header.c.
