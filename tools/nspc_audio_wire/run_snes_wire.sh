#!/bin/bash
# Device-ready zero-copy generic N-SPC HLE on the canonical hard-float M7 rig.
# Generates every derived source from the current tree; no stale /tmp input is
# required. WIRE_OFF=1 builds the same source set but leaves the SPC700 LLE on.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_wire.sh <rom.smc> [frames]}"
FRAMES="${2:-1500}"

SM=external/sm
RIG=tools/m7_qemu_rig
HERE=tools/nspc_audio_wire
NHLE=tools/nspc_hle
BUILD=/tmp/nspc_wire_build
GEN="$BUILD/gen"
OUT="$BUILD/rig"
mkdir -p "$GEN" "$OUT"

# Same zero-copy/dialect generator consumed by the firmware build.
python3 "$HERE/gen_nspc_wire.py" "$SM/src/spc_player.c" \
    "$SM/src/spc_player.h" "$SM/src/snes/apu.c" "$GEN"

# Generate the missing wire-aware rig from the canonical rig. Exact anchors
# make canonical-rig drift fail loudly instead of silently testing stock LLE.
python3 - "$RIG/rig_snes.c" "$OUT/rig_snes_wire.c" <<'PY'
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
bool wire_configure_rom(const uint8_t *rom, uint32_t len);
void wire_debug_dump(int frame);
extern int g_wire_on, g_wire_enable;
extern const char *g_wire_variant;
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
    wire_try_swap(snes, frame);
#ifdef RIG_WIRE_DEBUG
    if ((frame % 10) == 0) wire_debug_dump(frame);
#endif'''
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

old = '''  if (!snes_loadRom(snes, rom, (int)rom_len)) { printf("unsupported ROM\\n"); return 1; }'''
new = '''  if (!snes_loadRom(snes, rom, (int)rom_len)) { printf("unsupported ROM\\n"); return 1; }
  wire_configure_rom(rom, rom_len);
#ifdef WIRE_OFF
  g_wire_enable = 0;
#endif'''
assert s.count(old) == 1, 'wire configure rig anchor miss'
s = s.replace(old, new)

old = '''#endif
  return 0;
}'''
new = '''#endif
  printf("[nspc-rig] wire_on=%d variant=%s\\n", g_wire_on, g_wire_variant);
  return 0;
}'''
assert s.count(old) == 1, 'wire result rig anchor miss'
s = s.replace(old, new)

pathlib.Path(sys.argv[2]).write_text(s)
PY

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DSNES_SPIN_SKIP -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} ${RIG_EXTRA_DEF:-}"
if [ "${WIRE_OFF:-0}" = 1 ]; then DEF="$DEF -DWIRE_OFF"; fi
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"
SMINC="-iquote $SM/src"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
  --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

SRCS="$SM/src/snes/cart.c $SM/src/snes/cpu.c $SM/src/snes/dma.c $SM/src/snes/dsp.c \
      $SM/src/snes/dsp1_hle.c $SM/src/snes/input.c $SM/src/snes/ppu.c \
      $SM/src/snes/rc_dispatch.c $SM/src/snes/snes.c $SM/src/snes/snes_other.c \
      $SM/src/snes/spc.c $SM/src/snes/spin_skip.c $SM/src/tracing.c \
      $RIG/rig_runtime_hf.c"

OBJS=""
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
$CC -c $ARCH $OPT $DEF $INC -iquote $SM/src/snes -w "$GEN/apu_wire.c" -o "$OUT/apu_wire.o"
$CC -c $ARCH $OPT $DEF $INC $SMINC -include "$NHLE/nspc_config.h" -I"$GEN" -w "$GEN/spc_player_gen.c" -o "$OUT/spc_player_gen.o"
$CC -c $ARCH $OPT $DEF $INC $SMINC -I"$NHLE" -w "$NHLE/nspc_variant.c" -o "$OUT/nspc_variant.o"
$CC -c $ARCH $OPT $DEF $INC $SMINC -I"$NHLE" -I"$GEN" -I"$HERE" -Itools/snes_survey -w "$HERE/nspc_wire.c" -o "$OUT/nspc_wire.o"
$CC -c $ARCH $OPT $DEF $INC -w "$OUT/rig_snes_wire.c" -o "$OUT/rig_snes_wire.o"
OBJS="$OBJS $OUT/apu_wire.o $OUT/spc_player_gen.o $OUT/nspc_variant.o $OUT/nspc_wire.o $OUT/rig_snes_wire.o $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes_wire.elf"

arm-none-eabi-size "$OUT/rig_snes_wire.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes_wire.elf"
