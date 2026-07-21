#!/usr/bin/env python3
"""Generate a zero-copy, dialect-parameterized N-SPC player + renamed APU source.

Combines three transforms that tools/nspc_hle/ uses for its host proof-of-concept
into a single firmware-ready generator:

  1. Address sed (build.sh): SM's 4 hardcoded ARAM addresses → NSPC_* macros
     so the engine reads song/instr/dir offsets from g_nspc_cfg at runtime.
  2. Dialect rewrites (gen_variant.py): 16 exact-anchor substitutions that route
     sequence-dialect decisions (vcmd dispatch, note translation, instrument
     stride, KonamiBase pointers, loop guards) through nspc_variant.c hooks.
  3. Zero-copy transform: the engine's inline uint8 ram[65536] becomes a pointer
     to the live APU ARAM, and SpcPlayer_Create becomes CreateWithState(ram, dsp)
     so no 64 KB memcpy or second ARAM allocation is needed on the device.

The submodule originals (external/sm/src/spc_player.{c,h}, snes/apu.c) are NEVER
edited — only generated copies in $(BUILD_DIR)/snes_nspc_hle/ are compiled.

Every replacement is an exact-anchor substitution that must occur exactly once;
a miss aborts the build (the sed-miss lesson: a silent no-op transform ships the
old behavior with a new name).
"""
import sys
from pathlib import Path

if len(sys.argv) != 5:
    raise SystemExit(
        "usage: gen_nspc_wire.py <spc_player.c> <spc_player.h> <apu.c> <outdir>"
    )
player_c_path, player_h_path, apu_c_path, outdir = map(Path, sys.argv[1:4 + 1])
outdir.mkdir(parents=True, exist_ok=True)

# ---------------------------------------------------------------------------
# 1. Generate spc_player_gen.h: zero-copy struct (ram[65536] → *ram)
# ---------------------------------------------------------------------------
hdr = player_h_path.read_text()
RAM_ANCHOR = "  uint8 ram[65536]; // rest of ram"
if hdr.count(RAM_ANCHOR) != 1:
    raise SystemExit("spc_player.h zero-copy anchor miss "
                     f"(count={hdr.count(RAM_ANCHOR)})")
hdr = hdr.replace(RAM_ANCHOR,
                  "  uint8 *ram; // zero-copy: live APU ARAM (set by CreateWithState)")
(outdir / "spc_player_gen.h").write_text(hdr)

# ---------------------------------------------------------------------------
# 2. Generate spc_player_gen.c: address sed + dialect rewrites + zero-copy
# ---------------------------------------------------------------------------
src = player_c_path.read_text()

# Redirect include to our generated header (so the zero-copy struct is used).
INCLUDE_OLD = '#include "spc_player.h"'
if src.count(INCLUDE_OLD) != 1:
    raise SystemExit("spc_player.c include anchor miss")
src = src.replace(INCLUDE_OLD, '#include "spc_player_gen.h"')

# --- 2a. Address sed (build.sh): 4 hardcoded ARAM addresses → NSPC_* macros ---
ADDR_REPLS = [
    ("instrument * 6 + 0x6c00",
     "instrument * 6 + NSPC_INSTR"),
    ("p->ram[0x5820 + (a - 1) * 2]",
     "p->ram[NSPC_SONGLIST + (a - 1) * 2]"),
    ("p->ram[0x581e]",
     "p->ram[NSPC_SONGCUR]"),
    ("Dsp_Write(p, DIR, 0x6d)",
     "Dsp_Write(p, DIR, NSPC_DIRPAGE)"),
]
for old, new in ADDR_REPLS:
    if src.count(old) != 1:
        raise SystemExit(f"address sed anchor miss: {old!r} (count={src.count(old)})")
    src = src.replace(old, new)

# --- 2b. Dialect rewrites (gen_variant.py): 16 exact-anchor substitutions ---
# These MUST be applied AFTER the address sed (some anchors reference NSPC_INSTR).
DIALECT_REPLS = [
    # --- instrument layout ---
    ("  const uint8 *ip = p->ram + instrument * 6 + NSPC_INSTR;",
     "  const uint8 *ip = p->ram + instrument * NSPC_STRIDE + NSPC_INSTR;"),
    ("  c->instrument_pitch_base = ip[4] << 8 | ip[5];",
     "  c->instrument_pitch_base = nspc_instr_pitch_base(p, ip);"),

    # --- KonamiBase: song start address ---
    ("      p->music_ptr_toplevel = WORD(p->ram[NSPC_SONGLIST + (a - 1) * 2]);",
     "      p->music_ptr_toplevel = NSPC_ADDR(WORD(p->ram[NSPC_SONGLIST + (a - 1) * 2]));"),

    # --- KonamiBase: playlist jump destination ---
    ("        if (p->block_count != 0)\n          p->music_ptr_toplevel = t;",
     "        if (p->block_count != 0)\n          p->music_ptr_toplevel = NSPC_ADDR(t);"),

    # --- KonamiBase: section -> 8 track pointers ---
    ("    for (int i = 0; i < 8; i++)\n"
     "      p->channel[i].pattern_order_ptr_for_chan = WORD(p->ram[t]), t += 2;",
     "    t = NSPC_ADDR(t);\n"
     "    for (int i = 0; i < 8; i++) {\n"
     "      uint16 tp_ = WORD(p->ram[t]); t += 2;\n"
     "      p->channel[i].pattern_order_ptr_for_chan = tp_ ? NSPC_ADDR(tp_) : 0;\n"
     "    }"),

    # --- vcmd dispatch: variant boundary, remap hook, note translation ---
    ("          if (cmd >= 0xe0) {\n"
     "            HandleEffect(p, c, cmd); \n"
     "            continue;\n"
     "          }",
     "          if (cmd >= NSPC_VCMD_START) {\n"
     "            cmd = nspc_remap_vcmd(p, c, cmd);\n"
     "            if (cmd) HandleEffect(p, c, cmd);\n"
     "            continue;\n"
     "          }\n"
     "          cmd = nspc_xlat_note(cmd);"),

    # --- dialect note tables ---
    ("              c->note_gate_off_fixedpt = kNoteGateOffPct[cmd >> 4 & 7];",
     "              c->note_gate_off_fixedpt = NSPC_GATE(cmd >> 4 & 7);"),
    ("              c->channel_volume_master = kNoteVol[cmd & 0xf];",
     "              c->channel_volume_master = NSPC_NVOL(cmd & 0xf);"),

    # --- CALL target (HandleEffect case 0xef) ---
    ("    c->pattern_start_ptr = p->ram[c->pattern_order_ptr_for_chan++] << 8 | arg;",
     "    c->pattern_start_ptr = NSPC_ADDR((uint16)(p->ram[c->pattern_order_ptr_for_chan++] << 8 | arg));"),

    # --- WantWriteKof readahead: raw-stream dialect + guard ---
    ("static bool WantWriteKof(SpcPlayer *p, Channel *c) {\n"
     "  int loops = c->subroutine_num_loops;\n"
     "  int ptr = c->pattern_order_ptr_for_chan;\n"
     "\n"
     "  for (;;) {",
     "static bool WantWriteKof(SpcPlayer *p, Channel *c) {\n"
     "  int loops = c->subroutine_num_loops;\n"
     "  int ptr = c->pattern_order_ptr_for_chan;\n"
     "\n"
     "  int guard_ = 0;\n"
     "  for (;;) {\n"
     "    if (++guard_ > 4096) return true;"),
    ("      if (cmd == 0xc8)\n        return false;",
     "      if (cmd == NSPC_TIE_OP)\n        return false;"),
    ("      if (cmd == 0xef) {\n"
     "        ptr = p->ram[ptr + 0] | p->ram[ptr + 1] << 8;\n"
     "      } else if (cmd >= 0xe0) {\n"
     "        ptr += kEffectByteLength[cmd - 0xe0];\n"
     "      } else {",
     "      if (cmd == NSPC_CALL_OP) {\n"
     "        ptr = NSPC_ADDR((uint16)(p->ram[ptr + 0] | p->ram[ptr + 1] << 8));\n"
     "      } else if (cmd >= NSPC_VCMD_START) {\n"
     "        ptr += NSPC_VLEN(cmd);\n"
     "      } else {"),

    # --- pitch-slide readahead after a note ---
    ("  if (c->pitch_slide_length || p->ram[c->pattern_order_ptr_for_chan] != 0xf9)",
     "  if (c->pitch_slide_length || p->ram[c->pattern_order_ptr_for_chan] != NSPC_PSLIDE_OP)"),

    # --- playlist-walk guard ---
    ("static void Music_HandleCmdFromSnes(SpcPlayer *p) {\n"
     "  Channel *c;\n"
     "  int t;",
     "static void Music_HandleCmdFromSnes(SpcPlayer *p) {\n"
     "  Channel *c;\n"
     "  int t;\n"
     "  int npg_ = 0;"),
    ("    for (;;) next_phrase: {",
     "    for (;;) next_phrase: { if (++npg_ > 4096) return;"),

    # --- fast-forward guard ---
    ("    do {\n"
     "      Music_HandleCmdFromSnes(p);\n"
     "    } while (p->fast_forward);",
     "    { int ffg_ = 0;\n"
     "    do {\n"
     "      Music_HandleCmdFromSnes(p);\n"
     "    } while (p->fast_forward && ++ffg_ < 2048); }"),
]
for old, new in DIALECT_REPLS:
    if src.count(old) != 1:
        raise SystemExit(f"dialect rewrite anchor miss: {old[:60]!r}... "
                         f"(count={src.count(old)})")
    src = src.replace(old, new)

# --- 2c. Zero-copy Create: accept live ARAM+DSP, skip dsp_init ---
CREATE_SIG = "SpcPlayer *SpcPlayer_Create(void) {"
if src.count(CREATE_SIG) != 1:
    raise SystemExit("SpcPlayer_Create signature anchor miss")
src = src.replace(
    CREATE_SIG,
    "SpcPlayer *SpcPlayer_CreateWithState(uint8 *ram, Dsp *dsp) {")

CREATE_BODY = ("  memset(p, 0, sizeof(SpcPlayer));\n"
               "  p->dsp = dsp_init(p->ram);\n"
               "  p->reg_write_history = 0;")
if src.count(CREATE_BODY) != 1:
    raise SystemExit("SpcPlayer_Create body anchor miss")
src = src.replace(
    CREATE_BODY,
    "  memset(p, 0, sizeof(SpcPlayer));\n"
    "  p->ram = ram;\n"
    "  p->dsp = dsp;\n"
    "  p->reg_write_history = 0;")

# --- 2d. Export Spc_Loop_Part1/Part2 (remove static) for wire_step_sample ---
LOOP1_STATIC = "static void Spc_Loop_Part1(SpcPlayer *p) {"
LOOP2_STATIC = "static void Spc_Loop_Part2(SpcPlayer *p, uint8 ticks) {"
MUSIC_CMD_STATIC = "static void Music_HandleCmdFromSnes(SpcPlayer *p) {"
if src.count(LOOP1_STATIC) != 1:
    raise SystemExit("Spc_Loop_Part1 static anchor miss")
if src.count(LOOP2_STATIC) != 1:
    raise SystemExit("Spc_Loop_Part2 static anchor miss")
if src.count(MUSIC_CMD_STATIC) != 1:
    raise SystemExit("Music_HandleCmdFromSnes static anchor miss")
src = src.replace(LOOP1_STATIC, "void Spc_Loop_Part1(SpcPlayer *p) {")
src = src.replace(LOOP2_STATIC, "void Spc_Loop_Part2(SpcPlayer *p, uint8 ticks) {")
# Exported so wire_swap() can bootstrap a freshly zero-copy-adopted player:
# tempo starts at 0 (memset), so Spc_Loop_Part2's own tempo-accumulator gate
# can never wrap on its own to call this the first time -- see nspc_wire.c's
# wire_swap() comment for the full explanation.
src = src.replace(MUSIC_CMD_STATIC, "void Music_HandleCmdFromSnes(SpcPlayer *p) {")

(outdir / "spc_player_gen.c").write_text(src)

# ---------------------------------------------------------------------------
# 3. Generate apu_wire.c: rename apu_run → apu_run_lle (same as SMW)
# ---------------------------------------------------------------------------
apu = apu_c_path.read_text()
APU_ANCHOR = "void apu_run(Apu* apu, int cyclesToRun) {"
if apu.count(APU_ANCHOR) != 1:
    raise SystemExit("apu.c apu_run anchor miss")
(outdir / "apu_wire.c").write_text(
    apu.replace(APU_ANCHOR, "void apu_run_lle(Apu* apu, int cyclesToRun) {"))

print(f"gen_nspc_wire: spc_player_gen.c/h + apu_wire.c generated in {outdir}")
print(f"  {len(ADDR_REPLS)} address rewrites + {len(DIALECT_REPLS)} dialect rewrites "
      f"+ zero-copy + Spc_Loop export")
