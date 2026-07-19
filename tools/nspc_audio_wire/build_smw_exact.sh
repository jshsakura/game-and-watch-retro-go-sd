#!/bin/bash
# Build the generic SNES emulator with SMW's exact native SPC player wired in.
# Generated copies keep both external/sm and external/smw submodules untouched.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

HERE=tools/nspc_audio_wire
OUT=/tmp/smw_exact_wire_build
mkdir -p "$OUT"
rm -f "$OUT"/*.o "$OUT"/wire_smw_host

# The original player embeds a second 64 KiB ARAM.  Generate a device-safe
# zero-copy variant that adopts the generic core's existing ARAM and DSP.
python3 - external/smw/src/smw_spc_player.c "$OUT/smw_spc_player_gen.c" <<'PY'
import pathlib, sys
src = pathlib.Path(sys.argv[1]).read_text()
repls = [
    ("  uint8 ram[65536]; // rest of ram", "  uint8 *ram; // zero-copy: live generic-core ARAM"),
    ("SpcPlayer *SmwSpcPlayer_Create(void) {", "SpcPlayer *SmwSpcPlayer_CreateWithState(uint8 *ram, Dsp *dsp) {"),
    ("  p->base.dsp = dsp_init(p->ram);", "  p->ram = ram;\n  p->base.dsp = dsp;"),
]
for old, new in repls:
    if src.count(old) != 1:
        raise SystemExit(f"SMW player generation anchor miss: {old!r}")
    src = src.replace(old, new)
# Raw mailbox uploads already wrote into the shared ARAM.  Export the same
# post-upload reset used by SmwSpcPlayer_Upload, without copying a second blob.
anchor = "static SmwSpcPlayer g_static_spcplayer;"
if src.count(anchor) != 1:
    raise SystemExit("SMW raw-upload export anchor miss")
helper = r'''
void SmwSpcPlayer_FinishRawUpload(SpcPlayer *p_in) {
  SmwSpcPlayer *p = (SmwSpcPlayer *)p_in;
  p->base.port_to_snes[0] = p->base.port_to_snes[1] =
      p->base.port_to_snes[2] = p->base.port_to_snes[3] = 0;
  p->is_chan_on = 0;
  p->smw_tempo_increase = 0;
  p->smw_pause_music = 0;
  p->smw_player_on_yoshi = 0;
  p->echo_channels = 0;
  memset(p->base.input_ports, 0, sizeof(p->base.input_ports));
  memset(p->last_value_from_snes, 0, sizeof(p->last_value_from_snes));
  memset(p->new_value_from_snes, 0, sizeof(p->new_value_from_snes));
  p->music_ptr_toplevel = 0;
  for (int i = 0; i < 8; i++)
    p->channel[i].pattern_cur_ptr = 0;
  Dsp_Write(p, FLG, 0x20);
}

'''
src = src.replace(anchor, helper + anchor)
pathlib.Path(sys.argv[2]).write_text(src)
print("SMW exact player: zero-copy ARAM/DSP generation OK")
PY

sed 's/^void apu_run(Apu\* apu, int cyclesToRun) {/void apu_run_lle(Apu* apu, int cyclesToRun) {/' \
  external/sm/src/snes/apu.c > "$OUT/apu_wire.c"
grep -q '^void apu_run_lle' "$OUT/apu_wire.c" || { echo "apu_run rename miss"; exit 1; }

CORE="-O2 -g -ffunction-sections -fdata-sections -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim"
SMINC="-iquote external/sm/src"
OBJS=""
for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  b="$(basename "$f")"
  [ "$b" = apu.c ] && continue
  o="$OUT/${b%.c}.o"
  gcc -c $CORE "$f" -o "$o"
  OBJS="$OBJS $o"
done
gcc -c $CORE -iquote external/sm/src/snes "$OUT/apu_wire.c" -o "$OUT/apu_wire.o"
gcc -c $CORE -Iexternal/smw/src "$OUT/smw_spc_player_gen.c" -o "$OUT/smw_spc_player_gen.o"
gcc -c $CORE $SMINC -Iexternal/smw/src -I"$HERE" -Itools/snes_survey \
  "$HERE/smw_exact_wire.c" -o "$OUT/smw_exact_wire.o"
gcc -c $CORE $SMINC -I"$HERE" "$HERE/host_main.c" -o "$OUT/host_main.o"

gcc -Wl,--gc-sections -o "$OUT/wire_smw_host" $OBJS \
  "$OUT/apu_wire.o" "$OUT/smw_spc_player_gen.o" \
  "$OUT/smw_exact_wire.o" "$OUT/host_main.o" -lm
size "$OUT/wire_smw_host"
echo "BUILD OK -> $OUT/wire_smw_host"
