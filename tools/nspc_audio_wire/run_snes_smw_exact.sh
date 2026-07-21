#!/bin/bash
# M7 instruction-count gate for SMW's exact native SPC wire.
# WIRE_OFF=1 builds the same source set but leaves the SPC700 LLE active.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_smw_exact.sh <smw.sfc> [frames]}"
FRAMES="${2:-2400}"
HERE=tools/nspc_audio_wire
RIG=tools/m7_qemu_rig
OUT=/tmp/smw_exact_wire_build/m7
mkdir -p "$OUT"

# Generate the zero-copy exact player + renamed LLE APU source.
bash "$HERE/build_smw_exact.sh" >/dev/null
GEN=/tmp/smw_exact_wire_build

# Generate the wire-aware rig from the current canonical SNES M7 rig so new
# timing, allocator and hash gates cannot silently drift from the baseline.
python3 - "$RIG/rig_snes.c" "$OUT/rig_snes_smw_exact.c" <<'PY'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
old = '''static Snes *g_the_snes;
void RtlApuWrite(uint32_t adr, uint8_t val) {
  snes_catchupApu(g_the_snes);
  g_the_snes->apu->inPorts[adr & 0x3] = val;
}'''
new = '''static Snes *g_the_snes;
void wire_apu_write(Snes *snes, uint32_t adr, uint8_t val);
int wire_try_swap(Snes *snes, int frame);
void wire_frame_audio(int16_t *buf, int n);
extern int g_wire_on, g_wire_enable;
void RtlApuWrite(uint32_t adr, uint8_t val) {
  wire_apu_write(g_the_snes, adr, val);
}'''
assert s.count(old) == 1, 'RtlApuWrite rig anchor miss'
s = s.replace(old, new)
old = '''    if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
      dsp_getSamples(snes->apu->dsp, g_audio, 16000 / 60, 1);
    }
    uint32_t t2 = rig_timer_now();'''
new = '''    if (g_wire_on) {
      wire_frame_audio(g_audio, 16000 / 60);
    } else if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
      dsp_getSamples(snes->apu->dsp, g_audio, 16000 / 60, 1);
    }
    uint32_t t2 = rig_timer_now();
    wire_try_swap(snes, frame);'''
assert s.count(old) == 1, 'audio rig anchor miss'
s = s.replace(old, new)
old = '''  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;'''
new = '''  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
#ifdef WIRE_OFF
  g_wire_enable = 0;
#endif'''
assert s.count(old) == 1, 'wire enable rig anchor miss'
s = s.replace(old, new)
pathlib.Path(sys.argv[2]).write_text(s)
PY

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} -DRIG_INPUT_TAP -DSNES_SPIN_SKIP ${RIG_EXTRA_DEF:-}"
if [ "${WIRE_OFF:-0}" = 1 ]; then DEF="$DEF -DWIRE_OFF"; fi
INC="-Iexternal/sm -I$RIG/shim -Itools/sm_harness/shim"
SMINC="-iquote external/sm/src"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
  --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

SRCS="external/sm/src/snes/cart.c external/sm/src/snes/cpu.c external/sm/src/snes/dma.c \
external/sm/src/snes/dsp.c external/sm/src/snes/dsp1_hle.c external/sm/src/snes/input.c \
external/sm/src/snes/ppu.c external/sm/src/snes/rc_dispatch.c external/sm/src/snes/snes.c \
external/sm/src/snes/snes_other.c external/sm/src/snes/spc.c external/sm/src/snes/spin_skip.c \
external/sm/src/tracing.c $RIG/rig_runtime_hf.c"
OBJS=""
for src in $SRCS; do
  obj="$OUT/$(basename "${src%.c}").o"
  $CC -c $ARCH $OPT $DEF $INC -w "$src" -o "$obj"
  OBJS="$OBJS $obj"
done
$CC -c $ARCH $OPT $DEF $INC -iquote external/sm/src/snes -w "$GEN/apu_wire.c" -o "$OUT/apu_wire.o"
$CC -c $ARCH $OPT $DEF $INC -Iexternal/smw/src -w "$GEN/smw_spc_player_gen.c" -o "$OUT/smw_spc_player_gen.o"
$CC -c $ARCH $OPT $DEF $INC $SMINC -Iexternal/smw/src -I"$HERE" -Itools/snes_survey \
  -w "$HERE/smw_exact_wire.c" -o "$OUT/smw_exact_wire.o"
$CC -c $ARCH $OPT $DEF $INC -w "$OUT/rig_snes_smw_exact.c" -o "$OUT/rig_snes_smw_exact.o"

$CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
  $OBJS "$OUT/apu_wire.o" "$OUT/smw_spc_player_gen.o" "$OUT/smw_exact_wire.o" \
  "$OUT/rig_snes_smw_exact.o" "$OUT/rom.o" -lm -o "$OUT/rig_snes_smw_exact.elf"
arm-none-eabi-size "$OUT/rig_snes_smw_exact.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
  -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes_smw_exact.elf"
