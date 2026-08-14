/* The DSP-2/3/4 refusal, against the header text the firmware compiles.
 *
 * The predicate lives in Core/Src/porting/snes/snes_dsp_variant.h and this
 * includes it, rather than restating the four strings -- a test that carries
 * its own copy of the thing it tests proves only that two copies agree.
 *
 * What it is guarding: DSP-1/2/3/4 share one romType, so a DSP-2 cart attaches
 * the DSP-1 HLE and then misbehaves in silence. The refusal only helps if the
 * fingerprint fires on those titles and on nothing else.
 */
#include <stdio.h>
#include <string.h>

#include "snes_dsp_variant.h"

static int fails;

static void expect(const char *title, bool want, const char *why)
{
    /* The caller folds non-ASCII to '.' and NUL-terminates at 21; do the same
     * here so the input is shaped like the real one. */
    char name[22];
    for (int i = 0; i < 21; i++) {
        unsigned char ch = (unsigned char)(title[i] ? title[i] : ' ');
        name[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
        if (!title[i]) { /* pad the rest with spaces, as a real header does */
            for (int j = i; j < 21; j++) name[j] = ' ';
            break;
        }
    }
    name[21] = 0;

    bool got = snes_title_needs_unsupported_dsp(name);
    if (got != want) {
        printf("  FAIL [%s] -> %s, expected %s (%s)\n",
               name, got ? "refused" : "allowed", want ? "refused" : "allowed", why);
        fails++;
    }
}

int main(void)
{
    printf("=== a DSP-2/3/4 cartridge is refused, a DSP-1 one is not ===\n");

    /* Refused: the three chips this tree has no HLE for. */
    expect("DUNGEON MASTER",       true, "DSP-2");
    expect("SD GUNDAM GX",         true, "DSP-3");
    expect("TOP GEAR 3000",        true, "DSP-4");
    expect("PLANET'S CHAMP TG3000", true, "DSP-4, the other regional title");

    /* Allowed: DSP-1 carts, which the HLE does implement. These are the ones a
     * false positive would break, and two of them are on the measurement card. */
    expect("SUPER MARIO KART",     false, "DSP-1");
    expect("PILOTWINGS",           false, "DSP-1");
    expect("SUZUKA 8 HOURS",       false, "DSP-1");
    expect("BATTLE RACERS",        false, "DSP-1");
    expect("SUPER METROID",        false, "no coprocessor at all");

    /* A title that merely shares a word must not trip it -- the strings are
     * long enough to be specific, and this is what pins that. */
    expect("DUNGEON EXPLORER",     false, "shares only the first word");
    expect("TOP GEAR",             false, "TOP GEAR and TOP GEAR 2 are DSP-less");
    expect("TOP GEAR 2",           false, "DSP-less");
    expect("GUNDAM WING",          false, "shares only 'GUNDAM'");

    if (fails) { printf("FAILED (%d)\n", fails); return 1; }
    printf("  OK   4 refused, 9 allowed\n");
    return 0;
}
