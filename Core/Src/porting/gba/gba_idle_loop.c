/* Where each game busy-waits, so the core can stop waiting with it.
 *
 * A GBA game does its work and then spins until VBlank. gpSP can throw away the
 * rest of the frame's cycles when the PC reaches a known busy-wait — but ONLY for
 * a game whose 4-character code is in its own gba_over.h table, and that table is
 * hand-maintained: incomplete, and in places wrong. A game it does not know
 * busy-waits through the entire frame. That is not "a bit slower"; it is the
 * difference between 74,000 cycles of real work and all 280,896 of them.
 *
 * gpSP exposes the target as a plain global (cpu.h:161), so this table overrides
 * it AFTER the cart is loaded rather than by patching the submodule. Two reasons
 * that is the right way round:
 *
 *   - It corrects gpSP's own mistakes. FireRed and LeafGreen are listed there at
 *     0x80008b2, where there is no loop at all; the real one is at 0x80008c6.
 *     (ReGBA has the same wrong value.) Pinball of the Dead is listed at
 *     0x800030 — a dropped digit; ROM space starts at 0x8000000.
 *   - It is generated, not authored. The addresses below were measured by running
 *     each ROM and watching the per-frame cycle count collapse; an address that
 *     does not make it collapse is simply wrong, whatever a detector claims. Do
 *     not hand-edit this file — regenerate it.
 *
 * The "work" column is real CPU cycles per frame with the skip active, out of a
 * 280,896-cycle frame. It is what a button-masher reached: intros and early play,
 * not a late-game boss. Several of these have scenes that peak at a full frame.
 *
 * Ruby and Sapphire are deliberately absent. They have no busy-wait loop to skip —
 * they idle through the BIOS (SWI 5 / SWI 2), which gpSP already fast-forwards
 * (cpu.cc:1499). They measure 74k cycles and 74% idle with no entry at all.
 */
#include "gba_idle_loop.h"

#include <string.h>

typedef struct {
    char     code[4];   /* the 4 bytes at rom[0xAC] — NOT NUL-terminated */
    uint32_t pc;        /* the backward BRANCH, which is what gpSP compares against */
} gba_idle_entry_t;

static const gba_idle_entry_t gba_idle_loops[] = {
    /* code      pc            game                                work/frame  idle */
    { {'A','A','M','J'}, 0x80003ce },  /* Castlevania: Circle of the Moon   76,204   73% */
    { {'B','R','I','J'}, 0x80013d4 },  /* Rhythm Tengoku                    74,288   74% */
    { {'B','D','T','E'}, 0x800065a },  /* Downtown Nekketsu Monogatari EX   79,848   72% */
    { {'B','P','E','K'}, 0x80008ce },  /* Pokemon Emerald (KR patch)        78,982   72% */
    { {'B','P','G','E'}, 0x80008c6 },  /* Pokemon LeafGreen  — gpSP says 0x80008b2, wrong */
    { {'B','P','R','E'}, 0x80008c6 },  /* Pokemon FireRed    — gpSP says 0x80008b2, wrong */
    { {'B','4','Z','J'}, 0x8000914 },  /* Mega Man Zero 4                  115,390   59% */
    { {'A','A','2','C'}, 0x80005ec },  /* Super Mario World (GBA)          137,185   51% */
    { {'B','Z','3','J'}, 0x80019c4 },  /* Mega Man Zero 3                  139,326   50% */
    { {'A','F','X','J'}, 0x8000428 },  /* Final Fantasy Tactics Advance    140,189   50% */
    { {'A','Z','W','J'}, 0x8000f5e },  /* WarioWare, Inc.                  143,508   49% */
    { {'A','P','D','E'}, 0x8000300 },  /* Pinball of the Dead — gpSP says 0x800030, wrong */
};

uint32_t gba_idle_loop_lookup(const char *gamepak_code)
{
    if (gamepak_code == NULL)
        return 0;

    for (unsigned i = 0; i < sizeof(gba_idle_loops) / sizeof(gba_idle_loops[0]); i++) {
        if (memcmp(gba_idle_loops[i].code, gamepak_code, 4) == 0)
            return gba_idle_loops[i].pc;
    }
    return 0;   /* not known — gpSP's own table, or none, stands */
}
