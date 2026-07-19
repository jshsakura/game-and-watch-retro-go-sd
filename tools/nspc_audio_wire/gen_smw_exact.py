#!/usr/bin/env python3
"""Generate zero-copy SMW player and renamed LakeSnes APU sources."""
from pathlib import Path
import sys

if len(sys.argv) != 4:
    raise SystemExit("usage: gen_smw_exact.py <smw_spc_player.c> <apu.c> <outdir>")
player_path, apu_path, outdir = map(Path, sys.argv[1:])
outdir.mkdir(parents=True, exist_ok=True)
src = player_path.read_text()
repls = [
    ("  uint8 ram[65536]; // rest of ram", "  uint8 *ram; // zero-copy: live generic-core ARAM"),
    ("SpcPlayer *SmwSpcPlayer_Create(void) {", "SpcPlayer *SmwSpcPlayer_CreateWithState(uint8 *ram, Dsp *dsp) {"),
    ("  p->base.dsp = dsp_init(p->ram);", "  p->ram = ram;\n  p->base.dsp = dsp;"),
]
for old, new in repls:
    if src.count(old) != 1:
        raise SystemExit(f"SMW player generation anchor miss: {old!r}")
    src = src.replace(old, new)
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
(outdir / "smw_spc_player_gen.c").write_text(src.replace(anchor, helper + anchor))

apu = apu_path.read_text()
old = "void apu_run(Apu* apu, int cyclesToRun) {"
if apu.count(old) != 1:
    raise SystemExit("apu_run generation anchor miss")
(outdir / "apu_wire.c").write_text(apu.replace(old, "void apu_run_lle(Apu* apu, int cyclesToRun) {"))
print("SMW exact player: zero-copy ARAM/DSP generation OK")
