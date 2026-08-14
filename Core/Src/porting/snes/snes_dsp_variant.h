#pragma once
/* Which DSP is behind the one encoding the header gives us.
 *
 * DSP-1/2/3/4 are the same NEC uPD77C25 with different microcode, and Nintendo
 * gave them all the same romType at $ffd6, so the header cannot tell them
 * apart. Only dsp1_hle.c exists in this tree. A DSP-2/3/4 cart attaching it
 * does not crash -- unknown commands are NOPs by design -- it draws wrong or
 * stops advancing, silently, which is the worst of the three outcomes.
 *
 * The title is the fingerprint. That is what snes9x, Mesen and bsnes all fall
 * back to when a ROM is not in their database, and it is affordable here
 * because exactly three commercial titles are not DSP-1 and the 21-byte ASCII
 * name is already parsed off the header. A whole-ROM hash is not an option in
 * this tree (ROM hacks and regional variants have to keep working) and a game
 * database does not fit in what is left of internal flash.
 *
 * The asymmetry is what makes this safe: missing a regional variant costs
 * nothing, because that cart then behaves exactly as it does today. A false
 * positive would need a DSP-1 game whose title contains one of these strings.
 *
 * This lives in a header so the test links the same text the firmware compiles
 * rather than a copy of it -- see tests/test_snes_dsp_variant.c.
 */
#include <stdbool.h>
#include <string.h>

/* name: the header's 21-byte title, NUL-terminated, non-ASCII already folded. */
static inline bool snes_title_needs_unsupported_dsp(const char *name)
{
    return strstr(name, "DUNGEON MASTER")   /* DSP-2 */
        || strstr(name, "SD GUNDAM GX")     /* DSP-3 */
        || strstr(name, "TOP GEAR 3000")    /* DSP-4 */
        || strstr(name, "PLANET'S CHAMP");  /* DSP-4, the other regional title */
}
