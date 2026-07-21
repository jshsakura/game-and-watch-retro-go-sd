#!/usr/bin/env python3
"""Post-process the generated spc_player copy (after build.sh's address sed) to
route sequence-dialect decisions through g_nspc_cfg / nspc_variant.c hooks.

Every replacement is an EXACT-ANCHOR substitution that must occur exactly once;
a miss aborts the build (the sed-miss lesson: a silent no-op transform ships the
old behavior with a new name).

What gets rewritten and why:
 - vcmd dispatch boundary + remap hook (variant opcode set -> standard 0xE0 set)
 - note translation hook (tie/rest/percussion encodings differ per dialect)
 - note velocity/gate tables (dialect tables differ)
 - instrument entry stride + pitch-base hook (6-byte / 5-byte / Konami tuning)
 - KonamiBase pointer conversion at every sequence-pointer fetch (song start,
   playlist jump, track pointers, CALL target, readahead CALL)
 - raw-stream opcode checks (tie / call / pitch-slide / vcmd-length readahead)
 - loop guards: a foreign stream read with the wrong dialect is garbage; the
   engine's for(;;) readahead, next_phrase playlist walk and fast-forward loop
   must not hang the harness.
"""
import sys

path = sys.argv[1]
src = open(path).read()

REPL = [
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

    # --- KonamiBase: section -> 8 track pointers (0 = channel off, stays 0) ---
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

    # --- pitch-slide readahead after a note (raw stream opcode) ---
    ("  if (c->pitch_slide_length || p->ram[c->pattern_order_ptr_for_chan] != 0xf9)",
     "  if (c->pitch_slide_length || p->ram[c->pattern_order_ptr_for_chan] != NSPC_PSLIDE_OP)"),

    # --- playlist-walk guard (garbage playlists must not spin forever) ---
    ("static void Music_HandleCmdFromSnes(SpcPlayer *p) {\n"
     "  Channel *c;\n"
     "  int t;",
     "static void Music_HandleCmdFromSnes(SpcPlayer *p) {\n"
     "  Channel *c;\n"
     "  int t;\n"
     "  int npg_ = 0;"),
    ("    for (;;) next_phrase: {",
     "    for (;;) next_phrase: { if (++npg_ > 4096) return;"),

    # --- fast-forward guard (a garbage stream can set it and never clear) ---
    ("    do {\n"
     "      Music_HandleCmdFromSnes(p);\n"
     "    } while (p->fast_forward);",
     "    { int ffg_ = 0;\n"
     "    do {\n"
     "      Music_HandleCmdFromSnes(p);\n"
     "    } while (p->fast_forward && ++ffg_ < 2048); }"),
]

missing = [old for old, _ in REPL if src.count(old) != 1]
if missing:
    for m in missing:
        sys.stderr.write("ANCHOR MISS (count=%d):\n%s\n---\n" % (src.count(m), m))
    sys.exit(1)

for old, new in REPL:
    src = src.replace(old, new)

open(path, "w").write(src)
print("gen_variant: %d replacements applied" % len(REPL))
