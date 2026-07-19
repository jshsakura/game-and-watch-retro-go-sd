#!/bin/bash
# Build reusable SNES cost-center ELFs.  ROMs are injected at run time with
# QEMU's loader, so this compile/link happens once for an entire ROM library.
set -euo pipefail
cd "$(dirname "$0")/../.."

FRAMES="${1:-300}"
SM=external/sm
RIG=tools/m7_qemu_rig
OUT="$RIG/build_cost"
mkdir -p "$OUT"

python3 - "$SM/src/snes/apu.c" "$OUT/apu_cost.c" <<'PY'
import sys
src = open(sys.argv[1], encoding="utf-8").read()
assert src.count("spc_runOpcode(apu->spc)") == 2
assert src.count("dsp_cycle(apu->dsp);") == 2
src = src.replace(
    "spc_runOpcode(apu->spc)",
    "({ uint32_t _t=rig_timer_now(); int _r=spc_runOpcode(apu->spc); "
    "g_spc_ticks += (uint32_t)(rig_timer_now()-_t); _r; })")
src = src.replace(
    "dsp_cycle(apu->dsp);",
    "{ uint32_t _t=rig_timer_now(); dsp_cycle(apu->dsp); "
    "g_dsp_ticks += (uint32_t)(rig_timer_now()-_t); g_dsp_calls++; }")
open(sys.argv[2], "w", encoding="utf-8").write(src)
PY

# Deep PPU copy: instrument static stage call sites without touching external/sm.
python3 - "$SM/src/snes/ppu.c" "$OUT/ppu_deep.c" <<'PY'
import sys

src = open(sys.argv[1], encoding="utf-8").read()

def one(old, new):
    global src
    count = src.count(old)
    assert count == 1, (old[:60], count)
    src = src.replace(old, new)

one(
    "    ppu->lineHasSprites = !ppu->forcedBlank && ppu_evaluateSprites(ppu, line - 1);",
    "    if (!ppu->forcedBlank) { uint32_t _pt=rig_timer_now(); "
    "ppu->lineHasSprites = ppu_evaluateSprites(ppu, line - 1); "
    "g_ppu_sprite_eval_ticks += (uint32_t)(rig_timer_now()-_pt); } "
    "else { ppu->lineHasSprites = false; }")

calls = [
    ("      PpuDrawBackground_4bpp(ppu, y, sub, 0, 0xc000, 0x8000);", 0),
    ("      PpuDrawBackground_4bpp(ppu, y, sub, 1, 0xb100, 0x7100);", 1),
    ("      PpuDrawBackground_2bpp(ppu, y, sub, 2, 0xf200, 0x1200);", 2),
    ("    PpuDrawBackground_mode7(ppu, y, sub, 0x5000);", 0),
]
for call, layer in calls:
    indent = call[:len(call) - len(call.lstrip())]
    body = call.strip()
    one(call, f"{indent}{{ uint32_t _pt=rig_timer_now(); {body} "
              f"g_ppu_bg_ticks[{layer}] += (uint32_t)(rig_timer_now()-_pt); }}")

for call in (
    "      PpuDrawSprites(ppu, y, sub, true);",
    "      PpuDrawSprites(ppu, y, sub, false);",
):
    indent = call[:len(call) - len(call.lstrip())]
    body = call.strip()
    one(call, f"{indent}{{ uint32_t _pt=rig_timer_now(); {body} "
              "g_ppu_sprite_draw_ticks += (uint32_t)(rig_timer_now()-_pt); }")

one(
    "static inline void ClearBackdrop(PpuPixelPrioBufs *buf) {\n"
    "  for (size_t i = 0; i != arraysize(buf->data); i += 4)\n"
    "    *(uint64*)&buf->data[i] = 0x0500050005000500;\n}",
    "static inline void ClearBackdrop(PpuPixelPrioBufs *buf) {\n"
    "  uint32_t _pt=rig_timer_now();\n"
    "  for (size_t i = 0; i != arraysize(buf->data); i += 4)\n"
    "    *(uint64*)&buf->data[i] = 0x0500050005000500;\n"
    "  g_ppu_clear_ticks += (uint32_t)(rig_timer_now()-_pt);\n}")

one(
    "  if (ppu->paletteDirty)\n"
    "    PpuRebuildPalette(ppu);   /* cgram or brightness moved since the last line */",
    "  if (ppu->paletteDirty) {\n"
    "    uint32_t _pt=rig_timer_now();\n"
    "    PpuRebuildPalette(ppu);   /* cgram or brightness moved since the last line */\n"
    "    g_ppu_palette_ticks += (uint32_t)(rig_timer_now()-_pt);\n"
    "  }")

one(
    "    if (math_enabled_cur == 0 || fixed_color == 0 && !ppu->halfColor && !rendered_subscreen) {",
    "    if (math_enabled_cur == 0 || fixed_color == 0 && !ppu->halfColor && !rendered_subscreen) {\n"
    "      uint32_t _prof_fast_t=rig_timer_now();")
one(
    "#endif\n    } else {\n      uint8 *half_color_map = ppu->halfColor ? ppu->brightnessMultHalf : ppu->brightnessMult;",
    "#endif\n"
    "      g_ppu_fast_ticks += (uint32_t)(rig_timer_now()-_prof_fast_t);\n"
    "      g_ppu_fast_pixels += right - left;\n"
    "    } else {\n"
    "      uint32_t _prof_math_t=rig_timer_now();\n"
    "      uint8 *half_color_map = ppu->halfColor ? ppu->brightnessMultHalf : ppu->brightnessMult;")

tail = "      } while (dst++, ++i < right);\n    }\n  } while (cw_clip_math >>= 1, ++windex < cwin.nr);"
assert tail in src
src = src.replace(
    tail,
    "      } while (dst++, ++i < right);\n"
    "      g_ppu_math_ticks += (uint32_t)(rig_timer_now()-_prof_math_t);\n"
    "      g_ppu_math_pixels += right - left;\n"
    "    }\n  } while (cw_clip_math >>= 1, ++windex < cwin.nr);",
    1)

open(sys.argv[2], "w", encoding="utf-8").write(src)
PY

cat > "$OUT/cost_defs.h" <<'EOF'
#ifndef COST_DEFS_H
#define COST_DEFS_H
#include <stdint.h>
extern uint64_t g_spc_ticks, g_dsp_ticks, g_dsp_calls;
extern uint64_t g_ppu_bg_ticks[3];
extern uint64_t g_ppu_sprite_eval_ticks, g_ppu_sprite_draw_ticks;
extern uint64_t g_ppu_clear_ticks, g_ppu_palette_ticks;
extern uint64_t g_ppu_fast_ticks, g_ppu_math_ticks, g_ppu_line_ticks;
extern uint64_t g_ppu_fast_pixels, g_ppu_math_pixels;
uint32_t rig_timer_now(void);
#endif
EOF

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
BASE_DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DSNES_SPIN_SKIP -DHEADLESS -DRIG_ROM_LOADER -DRIG_COST_PROF -DRIG_DEVICE_VIDEO -DRIG_INPUT_TAP -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-100}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"
SRCS="$SM/src/snes/cart.c $SM/src/snes/cpu.c $SM/src/snes/dma.c \
      $SM/src/snes/dsp.c $SM/src/snes/input.c $SM/src/snes/ppu.c \
      $SM/src/snes/snes.c $SM/src/snes/snes_other.c $SM/src/snes/spc.c \
      $SM/src/snes/spin_skip.c $SM/src/snes/rc_dispatch.c $SM/src/tracing.c $RIG/rig_runtime_hf.c \
      $RIG/rig_snes.c"

build_one() {
  local name=$1 extra=$2 dir="$OUT/$1"
  mkdir -p "$dir"
  local defs="$BASE_DEF $extra" objs=""
  $CC -c $ARCH $OPT $defs $INC -iquote "$SM/src/snes" -iquote "$SM/src" \
      -include "$OUT/cost_defs.h" -w "$OUT/apu_cost.c" -o "$dir/apu_cost.o"
  objs="$dir/apu_cost.o"
  for src in $SRCS; do
    local obj="$dir/$(basename "${src%.c}").o"
    $CC -c $ARCH $OPT $defs $INC -w "$src" -o "$obj"
    objs="$objs $obj"
  done
  $CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
      $objs -lm -o "$OUT/snes_cost_$name.elf"
}

build_one on ""
build_one off "-DRIG_FRAMESKIP"

build_deep() {
  local dir="$OUT/deep" defs="$BASE_DEF -DRIG_PPU_DEEP" objs=""
  mkdir -p "$dir"
  $CC -c $ARCH $OPT $defs $INC -iquote "$SM/src/snes" -iquote "$SM/src" \
      -include "$OUT/cost_defs.h" -w "$OUT/apu_cost.c" -o "$dir/apu_cost.o"
  objs="$dir/apu_cost.o"
  $CC -c $ARCH $OPT $defs $INC -iquote "$SM/src/snes" -iquote "$SM/src" \
      -include "$OUT/cost_defs.h" -w "$OUT/ppu_deep.c" -o "$dir/ppu_deep.o"
  objs="$objs $dir/ppu_deep.o"
  for src in $SRCS; do
    [[ "$src" == "$SM/src/snes/ppu.c" ]] && continue
    local obj="$dir/$(basename "${src%.c}").o"
    $CC -c $ARCH $OPT $defs $INC -w "$src" -o "$obj"
    objs="$objs $obj"
  done
  $CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
      $objs -lm -o "$OUT/snes_ppu_deep.elf"
}

build_deep
arm-none-eabi-size "$OUT/snes_cost_on.elf" "$OUT/snes_cost_off.elf"
arm-none-eabi-size "$OUT/snes_ppu_deep.elf"
printf '%s\n' "$FRAMES" > "$OUT/frames.txt"
printf 'SNES cost ELFs ready: %s frames per ROM\n' "$FRAMES"
