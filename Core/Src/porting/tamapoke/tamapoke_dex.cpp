/* Gen-1 species table, with the names deliberately absent.
 *
 * Upstream bakes all 151 names into the binary. Those are trademarks, and a
 * source tree that carries them cannot be shared even though every line of
 * code in it is ours or MIT. So the numbers stay -- evolution, rarity, base
 * stats, biome, accent colour -- and the names are loaded at startup from
 * /mons/names.bin on the card, which the user generates from their own
 * checkout of upstream. The repository ends up with no Pokemon content at all.
 *
 * DEX_TBL is mutable for exactly that reason: load_names() points each entry's
 * name at the loaded blob. Nothing else writes to it.
 */
#include "dex.h"

#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "tamapoke_assets.h"
#include "tamapoke_sprites.h"  /* sdReady */

#define NAMES_MAGIC "TNAM"
#define NAMES_MAX 4096

/* Shown when the card has no names.bin: the pet is still perfectly playable,
 * it just goes by number. Failing loudly here would make a missing optional
 * asset look like a crash. */
static char g_fallback[DEX_COUNT + 1][8];
static char g_names_blob[NAMES_MAX];

DexEntry DEX_TBL[DEX_COUNT + 1] = {
    { nullptr, 0, 0, 0, 0x2946, 50, 50, 50, 50, 0 },  // 0: sin usar
    { nullptr, 2, 16, R_COMUN, 0x3C49, 45, 49, 49, 45, 2 },  // 1 planta
    { nullptr, 3, 32, R_EVO, 0x3C49, 60, 62, 63, 60, 2 },  // 2 planta
    { nullptr, 0, 0, R_EVO, 0x3C49, 80, 82, 83, 80, 2 },  // 3 planta
    { nullptr, 5, 16, R_COMUN, 0xEA87, 39, 52, 43, 65, 3 },  // 4 fuego
    { nullptr, 6, 36, R_EVO, 0xEA87, 58, 64, 58, 80, 3 },  // 5 fuego
    { nullptr, 0, 0, R_EVO, 0xEA87, 78, 84, 78, 100, 3 },  // 6 fuego
    { nullptr, 8, 16, R_COMUN, 0x4C98, 44, 48, 65, 43, 1 },  // 7 agua
    { nullptr, 9, 36, R_EVO, 0x4C98, 59, 63, 80, 58, 1 },  // 8 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 79, 83, 100, 78, 1 },  // 9 agua
    { nullptr, 11, 7, R_COMUN, 0x7CC4, 45, 30, 35, 45, 2 },  // 10 bicho
    { nullptr, 12, 10, R_EVO, 0x7CC4, 50, 20, 55, 30, 2 },  // 11 bicho
    { nullptr, 0, 0, R_EVO, 0x7CC4, 60, 45, 50, 70, 2 },  // 12 bicho
    { nullptr, 14, 7, R_COMUN, 0x7CC4, 40, 35, 30, 50, 2 },  // 13 bicho
    { nullptr, 15, 10, R_EVO, 0x7CC4, 45, 25, 50, 35, 2 },  // 14 bicho
    { nullptr, 0, 0, R_EVO, 0x7CC4, 65, 90, 40, 75, 2 },  // 15 bicho
    { nullptr, 17, 18, R_COMUN, 0x8C4D, 40, 45, 40, 56, 0 },  // 16 normal
    { nullptr, 18, 36, R_EVO, 0x8C4D, 63, 60, 55, 71, 0 },  // 17 normal
    { nullptr, 0, 0, R_EVO, 0x8C4D, 83, 80, 75, 101, 0 },  // 18 normal
    { nullptr, 20, 20, R_COMUN, 0x8C4D, 30, 56, 35, 72, 0 },  // 19 normal
    { nullptr, 0, 0, R_EVO, 0x8C4D, 55, 81, 60, 97, 0 },  // 20 normal
    { nullptr, 22, 20, R_COMUN, 0x8C4D, 40, 60, 30, 70, 0 },  // 21 normal
    { nullptr, 0, 0, R_EVO, 0x8C4D, 65, 90, 65, 100, 0 },  // 22 normal
    { nullptr, 24, 22, R_COMUN, 0x8A73, 35, 60, 44, 55, 0 },  // 23 veneno
    { nullptr, 0, 0, R_EVO, 0x8A73, 60, 95, 69, 80, 0 },  // 24 veneno
    { nullptr, 26, 30, R_COMUN, 0xBCA1, 35, 55, 40, 90, 0 },  // 25 electrico
    { nullptr, 0, 0, R_EVO, 0xBCA1, 60, 90, 55, 110, 0 },  // 26 electrico
    { nullptr, 28, 22, R_COMUN, 0xB447, 50, 75, 85, 40, 4 },  // 27 tierra
    { nullptr, 0, 0, R_EVO, 0xB447, 75, 100, 110, 65, 4 },  // 28 tierra
    { nullptr, 30, 16, R_COMUN, 0x8A73, 55, 47, 52, 41, 0 },  // 29 veneno
    { nullptr, 31, 30, R_EVO, 0x8A73, 70, 62, 67, 56, 0 },  // 30 veneno
    { nullptr, 0, 0, R_EVO, 0x8A73, 90, 92, 87, 76, 0 },  // 31 veneno
    { nullptr, 33, 16, R_COMUN, 0x8A73, 46, 57, 40, 50, 0 },  // 32 veneno
    { nullptr, 34, 30, R_EVO, 0x8A73, 61, 72, 57, 65, 0 },  // 33 veneno
    { nullptr, 0, 0, R_EVO, 0x8A73, 81, 102, 77, 85, 0 },  // 34 veneno
    { nullptr, 36, 30, R_COMUN, 0x8C4D, 70, 45, 48, 35, 0 },  // 35 normal
    { nullptr, 0, 0, R_EVO, 0x8C4D, 95, 70, 73, 60, 0 },  // 36 normal
    { nullptr, 38, 30, R_COMUN, 0xEA87, 38, 41, 40, 65, 3 },  // 37 fuego
    { nullptr, 0, 0, R_EVO, 0xEA87, 73, 76, 75, 100, 3 },  // 38 fuego
    { nullptr, 40, 30, R_COMUN, 0x8C4D, 115, 45, 20, 20, 0 },  // 39 normal
    { nullptr, 0, 0, R_EVO, 0x8C4D, 140, 70, 45, 45, 0 },  // 40 normal
    { nullptr, 42, 22, R_COMUN, 0x8A73, 40, 45, 35, 55, 0 },  // 41 veneno
    { nullptr, 0, 0, R_EVO, 0x8A73, 75, 80, 70, 90, 0 },  // 42 veneno
    { nullptr, 44, 21, R_COMUN, 0x3C49, 45, 50, 55, 30, 2 },  // 43 planta
    { nullptr, 45, 36, R_EVO, 0x3C49, 60, 65, 70, 40, 2 },  // 44 planta
    { nullptr, 0, 0, R_EVO, 0x3C49, 75, 80, 85, 50, 2 },  // 45 planta
    { nullptr, 47, 24, R_COMUN, 0x7CC4, 35, 70, 55, 25, 2 },  // 46 bicho
    { nullptr, 0, 0, R_EVO, 0x7CC4, 60, 95, 80, 30, 2 },  // 47 bicho
    { nullptr, 49, 31, R_COMUN, 0x7CC4, 60, 55, 50, 45, 2 },  // 48 bicho
    { nullptr, 0, 0, R_EVO, 0x7CC4, 70, 65, 60, 90, 2 },  // 49 bicho
    { nullptr, 51, 26, R_COMUN, 0xB447, 10, 55, 25, 95, 4 },  // 50 tierra
    { nullptr, 0, 0, R_EVO, 0xB447, 35, 100, 50, 120, 4 },  // 51 tierra
    { nullptr, 53, 28, R_COMUN, 0x8C4D, 40, 45, 35, 90, 0 },  // 52 normal
    { nullptr, 0, 0, R_EVO, 0x8C4D, 65, 70, 60, 115, 0 },  // 53 normal
    { nullptr, 55, 33, R_COMUN, 0x4C98, 50, 52, 48, 55, 1 },  // 54 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 80, 82, 78, 85, 1 },  // 55 agua
    { nullptr, 57, 28, R_COMUN, 0xA2A5, 40, 80, 35, 70, 0 },  // 56 lucha
    { nullptr, 0, 0, R_EVO, 0xA2A5, 65, 105, 60, 95, 0 },  // 57 lucha
    { nullptr, 59, 30, R_RARO, 0xEA87, 55, 70, 45, 60, 3 },  // 58 fuego
    { nullptr, 0, 0, R_EVO, 0xEA87, 90, 110, 80, 95, 3 },  // 59 fuego
    { nullptr, 61, 25, R_COMUN, 0x4C98, 40, 50, 40, 90, 1 },  // 60 agua
    { nullptr, 62, 36, R_EVO, 0x4C98, 65, 65, 65, 90, 1 },  // 61 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 90, 95, 95, 70, 1 },  // 62 agua
    { nullptr, 64, 16, R_COMUN, 0xD28F, 25, 20, 15, 90, 0 },  // 63 psiquico
    { nullptr, 65, 40, R_EVO, 0xD28F, 40, 35, 30, 105, 0 },  // 64 psiquico
    { nullptr, 0, 0, R_EVO, 0xD28F, 55, 50, 45, 120, 0 },  // 65 psiquico
    { nullptr, 67, 28, R_COMUN, 0xA2A5, 70, 80, 50, 35, 0 },  // 66 lucha
    { nullptr, 68, 40, R_EVO, 0xA2A5, 80, 100, 70, 45, 0 },  // 67 lucha
    { nullptr, 0, 0, R_EVO, 0xA2A5, 90, 130, 80, 55, 0 },  // 68 lucha
    { nullptr, 70, 21, R_COMUN, 0x3C49, 50, 75, 35, 40, 2 },  // 69 planta
    { nullptr, 71, 36, R_EVO, 0x3C49, 65, 90, 50, 55, 2 },  // 70 planta
    { nullptr, 0, 0, R_EVO, 0x3C49, 80, 105, 65, 70, 2 },  // 71 planta
    { nullptr, 73, 30, R_COMUN, 0x4C98, 40, 40, 35, 70, 1 },  // 72 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 80, 70, 65, 100, 1 },  // 73 agua
    { nullptr, 75, 25, R_COMUN, 0x9407, 40, 80, 100, 20, 4 },  // 74 roca
    { nullptr, 76, 40, R_EVO, 0x9407, 55, 95, 115, 35, 4 },  // 75 roca
    { nullptr, 0, 0, R_EVO, 0x9407, 80, 120, 130, 45, 4 },  // 76 roca
    { nullptr, 78, 40, R_RARO, 0xEA87, 50, 85, 55, 90, 3 },  // 77 fuego
    { nullptr, 0, 0, R_EVO, 0xEA87, 65, 100, 70, 105, 3 },  // 78 fuego
    { nullptr, 80, 37, R_COMUN, 0x4C98, 90, 65, 65, 15, 1 },  // 79 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 95, 75, 110, 30, 1 },  // 80 agua
    { nullptr, 82, 30, R_COMUN, 0xBCA1, 25, 35, 70, 45, 0 },  // 81 electrico
    { nullptr, 0, 0, R_EVO, 0xBCA1, 50, 60, 95, 70, 0 },  // 82 electrico
    { nullptr, 0, 0, R_RARO, 0x8C4D, 52, 90, 55, 60, 0 },  // 83 normal
    { nullptr, 85, 31, R_COMUN, 0x8C4D, 35, 85, 45, 75, 0 },  // 84 normal
    { nullptr, 0, 0, R_EVO, 0x8C4D, 60, 110, 70, 110, 0 },  // 85 normal
    { nullptr, 87, 34, R_COMUN, 0x4C98, 65, 45, 55, 45, 1 },  // 86 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 90, 70, 80, 70, 1 },  // 87 agua
    { nullptr, 89, 38, R_RARO, 0x8A73, 80, 80, 50, 25, 0 },  // 88 veneno
    { nullptr, 0, 0, R_EVO, 0x8A73, 105, 105, 75, 50, 0 },  // 89 veneno
    { nullptr, 91, 30, R_COMUN, 0x4C98, 30, 65, 100, 40, 1 },  // 90 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 50, 95, 180, 70, 1 },  // 91 agua
    { nullptr, 93, 25, R_COMUN, 0x6AD3, 30, 35, 30, 80, 0 },  // 92 fantasma
    { nullptr, 94, 40, R_EVO, 0x6AD3, 45, 50, 45, 95, 0 },  // 93 fantasma
    { nullptr, 0, 0, R_EVO, 0x6AD3, 60, 65, 60, 110, 0 },  // 94 fantasma
    { nullptr, 0, 0, R_RARO, 0x9407, 35, 45, 160, 70, 4 },  // 95 roca
    { nullptr, 97, 26, R_COMUN, 0xD28F, 60, 48, 45, 42, 0 },  // 96 psiquico
    { nullptr, 0, 0, R_EVO, 0xD28F, 85, 73, 70, 67, 0 },  // 97 psiquico
    { nullptr, 99, 28, R_COMUN, 0x4C98, 30, 105, 90, 50, 1 },  // 98 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 55, 130, 115, 75, 1 },  // 99 agua
    { nullptr, 101, 30, R_COMUN, 0xBCA1, 40, 30, 50, 100, 0 },  // 100 electrico
    { nullptr, 0, 0, R_EVO, 0xBCA1, 60, 50, 70, 150, 0 },  // 101 electrico
    { nullptr, 103, 30, R_COMUN, 0x3C49, 60, 40, 80, 40, 2 },  // 102 planta
    { nullptr, 0, 0, R_EVO, 0x3C49, 95, 95, 85, 55, 2 },  // 103 planta
    { nullptr, 105, 28, R_COMUN, 0xB447, 50, 50, 95, 35, 4 },  // 104 tierra
    { nullptr, 0, 0, R_EVO, 0xB447, 60, 80, 110, 45, 4 },  // 105 tierra
    { nullptr, 0, 0, R_RARO, 0xA2A5, 50, 120, 53, 87, 0 },  // 106 lucha
    { nullptr, 0, 0, R_RARO, 0xA2A5, 50, 105, 79, 76, 0 },  // 107 lucha
    { nullptr, 0, 0, R_RARO, 0x8C4D, 90, 55, 75, 30, 0 },  // 108 normal
    { nullptr, 110, 35, R_COMUN, 0x8A73, 40, 65, 95, 35, 0 },  // 109 veneno
    { nullptr, 0, 0, R_EVO, 0x8A73, 65, 90, 120, 60, 0 },  // 110 veneno
    { nullptr, 112, 42, R_RARO, 0xB447, 80, 85, 95, 25, 4 },  // 111 tierra
    { nullptr, 0, 0, R_EVO, 0xB447, 105, 130, 120, 40, 4 },  // 112 tierra
    { nullptr, 0, 0, R_RARO, 0x8C4D, 250, 5, 5, 50, 0 },  // 113 normal
    { nullptr, 0, 0, R_RARO, 0x3C49, 65, 55, 115, 60, 2 },  // 114 planta
    { nullptr, 0, 0, R_RARO, 0x8C4D, 105, 95, 80, 90, 0 },  // 115 normal
    { nullptr, 117, 32, R_COMUN, 0x4C98, 30, 40, 70, 60, 1 },  // 116 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 55, 65, 95, 85, 1 },  // 117 agua
    { nullptr, 119, 33, R_COMUN, 0x4C98, 45, 67, 60, 63, 1 },  // 118 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 80, 92, 65, 68, 1 },  // 119 agua
    { nullptr, 121, 30, R_COMUN, 0x4C98, 30, 45, 55, 85, 1 },  // 120 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 60, 75, 85, 115, 1 },  // 121 agua
    { nullptr, 0, 0, R_RARO, 0xD28F, 40, 45, 65, 90, 0 },  // 122 psiquico
    { nullptr, 0, 0, R_RARO, 0x7CC4, 70, 110, 80, 105, 2 },  // 123 bicho
    { nullptr, 0, 0, R_RARO, 0x4DB8, 65, 50, 35, 95, 5 },  // 124 hielo
    { nullptr, 0, 0, R_RARO, 0xBCA1, 65, 83, 57, 105, 0 },  // 125 electrico
    { nullptr, 0, 0, R_RARO, 0xEA87, 65, 95, 57, 93, 3 },  // 126 fuego
    { nullptr, 0, 0, R_RARO, 0x7CC4, 65, 125, 100, 85, 2 },  // 127 bicho
    { nullptr, 0, 0, R_RARO, 0x8C4D, 75, 100, 95, 110, 0 },  // 128 normal
    { nullptr, 130, 20, R_COMUN, 0x4C98, 20, 10, 55, 80, 1 },  // 129 agua
    { nullptr, 0, 0, R_EVO, 0x4C98, 95, 125, 79, 81, 1 },  // 130 agua
    { nullptr, 0, 0, R_RARO, 0x4C98, 130, 85, 80, 60, 1 },  // 131 agua
    { nullptr, 0, 0, R_RARO, 0x8C4D, 48, 48, 48, 48, 0 },  // 132 normal
    { nullptr, 134, 30, R_COMUN, 0x8C4D, 55, 55, 50, 55, 0 },  // 133 normal
    { nullptr, 0, 0, R_EVO, 0x4C98, 130, 65, 60, 65, 1 },  // 134 agua
    { nullptr, 0, 0, R_EVO, 0xBCA1, 65, 65, 60, 130, 0 },  // 135 electrico
    { nullptr, 0, 0, R_EVO, 0xEA87, 65, 130, 60, 65, 3 },  // 136 fuego
    { nullptr, 0, 0, R_RARO, 0x8C4D, 65, 60, 70, 40, 0 },  // 137 normal
    { nullptr, 139, 40, R_RARO, 0x9407, 35, 40, 100, 35, 1 },  // 138 roca
    { nullptr, 0, 0, R_EVO, 0x9407, 70, 60, 125, 55, 1 },  // 139 roca
    { nullptr, 141, 40, R_RARO, 0x9407, 30, 80, 90, 55, 1 },  // 140 roca
    { nullptr, 0, 0, R_EVO, 0x9407, 60, 115, 105, 80, 1 },  // 141 roca
    { nullptr, 0, 0, R_RARO, 0x9407, 80, 105, 65, 130, 4 },  // 142 roca
    { nullptr, 0, 0, R_RARO, 0x8C4D, 160, 110, 65, 30, 0 },  // 143 normal
    { nullptr, 0, 0, R_LEGENDARIO, 0x4DB8, 90, 85, 100, 85, 5 },  // 144 hielo
    { nullptr, 0, 0, R_LEGENDARIO, 0xBCA1, 90, 90, 85, 100, 0 },  // 145 electrico
    { nullptr, 0, 0, R_LEGENDARIO, 0xEA87, 90, 100, 90, 90, 3 },  // 146 fuego
    { nullptr, 148, 30, R_RARO, 0x5A98, 41, 64, 45, 50, 1 },  // 147 dragon
    { nullptr, 149, 55, R_EVO, 0x5A98, 61, 84, 65, 70, 1 },  // 148 dragon
    { nullptr, 0, 0, R_EVO, 0x5A98, 91, 134, 95, 80, 1 },  // 149 dragon
    { nullptr, 0, 0, R_LEGENDARIO, 0xD28F, 106, 110, 90, 130, 0 },  // 150 psiquico
    { nullptr, 0, 0, R_LEGENDARIO, 0xD28F, 100, 100, 100, 100, 0 },  // 151 psiquico
};

/* names.bin: "TNAM", u16 count, then count NUL-terminated strings in dex order
 * starting at #1. Written by tools/tamapoke/make_dex_names.py.
 *
 * One table per language, because a Korean UI showing BULBASAUR is not
 * localised. English comes from upstream's own table; Korean comes from
 * PokeAPI, which is where upstream already gets its base stats. Called again
 * whenever the language changes -- the strings are pointers into one blob, so
 * switching means reloading it. */
bool tamapoke_dex_load_names(void) {
    const char *entry = (gLang == LANG_KO) ? "names_ko.bin" : "names.bin";

    for (int i = 0; i <= DEX_COUNT; i++) {
        snprintf(g_fallback[i], sizeof(g_fallback[i]), "#%03d", i);
        DEX_TBL[i].name = g_fallback[i];
    }
    if (!sdReady) return false;

    uint32_t got = tamapoke_assets_read(entry, (uint8_t *)g_names_blob,
                                       sizeof(g_names_blob));
    if (!got && gLang == LANG_KO) {  /* no Korean table on this card */
        got = tamapoke_assets_read("names.bin", (uint8_t *)g_names_blob,
                                   sizeof(g_names_blob));
    }
    if (got < 6 || memcmp(g_names_blob, NAMES_MAGIC, 4) != 0) return false;

    uint16_t count;
    memcpy(&count, g_names_blob + 4, 2);
    if (count > DEX_COUNT) count = DEX_COUNT;

    const char *p = g_names_blob + 6;
    const char *end = g_names_blob + got;
    for (uint16_t i = 1; i <= count && p < end; i++) {
        DEX_TBL[i].name = p;
        p += strlen(p) + 1;
    }
    return true;
}
