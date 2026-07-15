/* Runtime parameterization of what Super Metroid hardcodes in spc_player.c, so
 * the same native N-SPC engine can play an arbitrary game's uploaded ARAM.
 * Force-included into the build-time-generated copy of spc_player.c (build.sh);
 * the submodule file is never edited.
 *
 * Two layers:
 *  - the 4 ARAM addresses (song list / current song / instrument table / DIR)
 *  - the sequence DIALECT: vcmd base + per-opcode remap to the standard 0xE0
 *    set the engine implements, raw operand lengths (for readahead/skip), note
 *    encoding boundaries (tie/rest/percussion), instrument entry stride, and
 *    the Konami base-address model (GD3: all sequence pointers are relative).
 *
 * Dialect data comes from VGMTrans (zlib) NinSnesProfile.cpp (ref/):
 *   standard = loadStandardVcmdMap(0xe0)              (what spc_player is)
 *   earlier  = SMW-era map at 0xda, REORDERED (pitch-slide at 0xdd, no
 *              per-channel transpose), 5-byte instruments, tie 0xc6 rest 0xc7,
 *              percussion 0xd0-0xd9
 *   konami   = standard 0xe0 base + GD3 overrides (loop start/end, ADSR+GAIN,
 *              several NOP/unknowns) + KonamiBase pointer model + per-SRCN
 *              tuning tables instead of bytes 4/5 of the instrument entry
 */
#ifndef NSPC_CONFIG_H
#define NSPC_CONFIG_H

struct NspcCfg {
  /* addresses */
  int instrTable;   /* was 0x6c00 */
  int songList;     /* was 0x5820 */
  int songCur;      /* was 0x581e (= songList-2) */
  int dirPage;      /* was 0x6d (DSP DIR page) */
  /* dialect */
  int vcmdStart;    /* first vcmd opcode: std 0xe0, earlier 0xda */
  int tieOp;        /* RAW tie opcode   (std 0xc8, earlier 0xc6) */
  int callOp;       /* RAW call opcode  (std 0xef, earlier 0xe9) */
  int pslideOp;     /* RAW pitch-slide  (std 0xf9, earlier 0xdd) */
  int stride;       /* instrument entry bytes: 6 std/konami, 5 earlier */
  int baseAddr;     /* KonamiBase: added to every sequence pointer (0 = direct) */
  int tunLow, tunCnt; /* GD3 per-SRCN tuning: coarse @tunLow, fine @tunLow+tunCnt */
  const unsigned char *remap;  /* [256] raw vcmd -> std vcmd (0 = skip/handled) */
  const unsigned char *vlen;   /* [256] RAW operand count per opcode */
  const unsigned char *nvol;   /* [16] note-velocity table (dialect) */
  const unsigned char *gate;   /* [8]  note-gate table (dialect) */
};
extern struct NspcCfg g_nspc_cfg;

/* hooks provided by nspc_variant.c */
struct SpcPlayer; struct Channel;
unsigned char nspc_xlat_note(unsigned char cmd);
unsigned char nspc_remap_vcmd(struct SpcPlayer *p, struct Channel *c, unsigned char cmd);
unsigned short nspc_instr_pitch_base(struct SpcPlayer *p, const unsigned char *ip);

#define NSPC_INSTR      (g_nspc_cfg.instrTable)
#define NSPC_SONGLIST   (g_nspc_cfg.songList)
#define NSPC_SONGCUR    (g_nspc_cfg.songCur)
#define NSPC_DIRPAGE    (g_nspc_cfg.dirPage)
#define NSPC_VCMD_START ((unsigned char)g_nspc_cfg.vcmdStart)
#define NSPC_TIE_OP     ((unsigned char)g_nspc_cfg.tieOp)
#define NSPC_CALL_OP    ((unsigned char)g_nspc_cfg.callOp)
#define NSPC_PSLIDE_OP  ((unsigned char)g_nspc_cfg.pslideOp)
#define NSPC_STRIDE     (g_nspc_cfg.stride)
#define NSPC_ADDR(x)    ((unsigned short)((x) + g_nspc_cfg.baseAddr))
#define NSPC_VLEN(c)    (g_nspc_cfg.vlen[(unsigned char)(c)])
#define NSPC_NVOL(i)    (g_nspc_cfg.nvol[(i)])
#define NSPC_GATE(i)    (g_nspc_cfg.gate[(i)])

#endif
