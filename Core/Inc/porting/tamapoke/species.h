#pragma once
#include <stdint.h>

// Names removed on purpose: they are trademarks, and this tree is meant to be
// shareable. The nullptr entries fall back to DEX_TBL, whose names are loaded
// from the card at startup (see dex.h). Numbers and sprite maps are generated
// by upstream tools/sprites.py:
//   python3 tools/sprites.py emit

#define SPRITE_W 32
#define SPRITE_H 32

#define UI_BG_DAY 0xF77C  // #f2efe1
#define UI_BG_NIGHT 0x10C5  // #141828
#define UI_INK 0x2946  // #2a2a36
#define UI_INK_NIGHT 0xDEFE  // #d8dcf0
#define UI_TRACK 0xDE97  // #d8d2bd
#define UI_BAR_OK 0x5DCD  // #58b868
#define UI_BAR_WARN 0xED07  // #e8a23c
#define UI_BAR_BAD 0xEA87  // #e8503a
/* The red berry, drawn from the green berry's shape with this tint. A deeper
 * crimson than UI_BAR_BAD (#e8503a), which is the same orange-red as the apple's
 * own sprite -- at 16px the two were telling apart only by the apple's leaf. Still
 * red, because the game calls this one the red berry (S_BERRY_RED); a purple would
 * read more distinctly and then contradict its own label. */
#define UI_BERRY_RED 0xB987  // #b8323a

#define UI_WHITE 0xFFFF  // #ffffff

enum ElementType : uint8_t { TYPE_FUEGO, TYPE_PLANTA, TYPE_AGUA };

enum : int8_t {
  SP_CHARMANDER = 0,
  SP_CHARMELEON,
  SP_CHARIZARD,
  SP_BULBASAUR,
  SP_IVYSAUR,
  SP_VENUSAUR,
  SP_SQUIRTLE,
  SP_WARTORTLE,
  SP_BLASTOISE,
  NUM_SPECIES
};


/* The nine starter-line maps used to sit here as 32x32 character art. They are
 * depictions of trademarked characters, so they live on the card now -- the
 * same move as the species names, and for the same reason: it is what lets
 * this source be published. SPR_EGG, SPR_POOP and SPR_HEART stay, because they
 * are upstream's own drawings of an egg, a turd and a heart.
 *
 * Loaded from the assets .dat at startup. Until then every row is a blank
 * line, so a missing file draws nothing rather than dereferencing null. */
#define FALLBACK_SPRITE_COUNT 9
#define FALLBACK_SPRITE_DIM   32
extern const char *SPR_FALLBACK[FALLBACK_SPRITE_COUNT][FALLBACK_SPRITE_DIM];
bool tamapoke_load_fallback_sprites(void);

struct Species {
  const char *name;
  ElementType type;
  const char *const *sprite;
  uint8_t scale;
  int8_t evolvesTo;
  uint8_t evolveLevel;
  uint8_t eyeRow, eyeColL, eyeColR;  // anclas para expresiones (ojos 3x4)
  uint8_t mouthRow, mouthCol;
  uint16_t bodyColor;  // para borrar ojos/boca al expresar
  uint16_t accent;     // color UI del tipo
};

// caracter de sprite -> RGB565
static inline uint16_t spriteColor(char ch) {
  switch (ch) {
    case 'k': return 0x18C4;  // #1b1b25
    case 'w': return 0xFFFF;  // #ffffff
    case 'y': return 0xFED2;  // #f8d990
    case 'Y': return 0xE5CC;  // #e0b860
    case 'o': return 0xF427;  // #f5863d
    case 'O': return 0xD2E5;  // #d65f28
    case 'r': return 0xEA87;  // #e8503a
    case 'R': return 0xB184;  // #b53224
    case 'f': return 0xFECB;  // #ffd95e
    case 't': return 0x8EB6;  // #8fd6b4
    case 'T': return 0x5D71;  // #5fae8c
    case 'g': return 0x5DCD;  // #58b868
    case 'G': return 0x3C49;  // #3c8a4c
    case 'd': return 0x3BEC;  // #3f7e62
    case 'p': return 0xF454;  // #f08aa4
    case 'P': return 0xC2F0;  // #c75f80
    case 'b': return 0x7E3D;  // #7cc4ea
    case 'B': return 0x4C98;  // #4f93c4
    case 'N': return 0x3B74;  // #3a6fa0
    case 'M': return 0x2A8F;  // #2a5278
    case 'c': return 0xB3C8;  // #b07a45
    case 'C': return 0x7AA6;  // #7e5530
    case 'l': return 0x9D5C;  // #9aa9e0
    case 'L': return 0x6BF7;  // #6f7cb8
    case 's': return 0xAD97;  // #aab0bc
    case 'S': return 0x7BF1;  // #787e8c
    default: return 0;
  }
}










static const char* const SPR_ICON_FOOD[16] = {  // 16x16
  "................",
  "................",
  "...........k....",
  "........k.kk....",
  "........k.......",
  ".......krk......",
  ".....kkrrrkk....",
  "....krwrrrrrk...",
  "....kwrrrrrrk...",
  "...krrrrrrrrRk..",
  "...krrrrrrrrRk..",
  "....krrrrrrrk...",
  "....krrrrrrRk...",
  ".....kkRRRkk....",
  ".......kkk......",
  "................",
};

static const char* const SPR_ICON_PLAY[16] = {  // 16x16
  "................",
  "................",
  ".......kkk......",
  ".....kkrrrkk....",
  "....krrrrrrrk...",
  "...krrrrrrrrrk..",
  "...krrrrrrrrrk..",
  "..krrrrkkkrrrRk.",
  "..kkkkkkwkkkkkk.",
  "..krrwwkkkwwrRk.",
  "...kwwwwwwwwwk..",
  "...kwwwwwwwwYk..",
  "...kwwwwwwwwYk..",
  "....kwwwwwwYk...",
  ".....kkkkkkk....",
  "................",
};

static const char* const SPR_ICON_LIGHT[16] = {  // 16x16
  "................",
  "................",
  "................",
  "......kk....f...",
  "....kkk.........",
  "....kfk......f..",
  "...kffk.........",
  "...kffk.........",
  "...kfffk........",
  "...kffffk.......",
  "...kfffffkk.kk..",
  "....kffffffkk...",
  "....kkfffffkk...",
  "......kkkkk.....",
  "................",
  "................",
};

static const char* const SPR_ICON_CLEAN[16] = {  // 16x16
  "................",
  "................",
  "................",
  "...kk......kkk..",
  "...kk.....kwbbk.",
  "...kk.....kbbbk.",
  "......kkk..kkk..",
  "....kkbbbkk.....",
  "...kbwbbbbbk....",
  "...kbbwbbbBk....",
  "...kbbbbbbbk....",
  "...kbbbbbbBk....",
  "...kbbbbbbBk....",
  "....kkBBBkk.....",
  "......kkk.......",
  "................",
};

static const char* const SPR_ICON_BERRY_B[16] = {  // 16x16
  "................",
  "................",
  "...........k....",
  "........k.k.....",
  "........k.......",
  ".......kbk......",
  ".....kkbbbkk....",
  "....kbwbbbbbk...",
  "....kwbbbbbbk...",
  "...kbbbbbbbbBk..",
  "...kbbbbbbbbBk..",
  "....kbbbbbbbk...",
  "....kbbbbbbBk...",
  ".....kkBBBkk....",
  ".......kkk......",
  "................",
};

static const char* const SPR_ICON_BERRY_G[16] = {  // 16x16
  "................",
  "................",
  "...........k....",
  "........k.k.....",
  "........k.......",
  ".......kgk......",
  ".....kkgggkk....",
  "....kgwgggggk...",
  "....kwggggggk...",
  "...kggggggggGk..",
  "...kggggggggGk..",
  "....kgggggggk...",
  "....kggggggGk...",
  ".....kkGGGkk....",
  ".......kkk......",
  "................",
};

static const char* const SPR_ICON_CANDY[16] = {  // 16x16
  "................",
  "................",
  "................",
  "................",
  "................",
  "......kkkkk.....",
  "..k..kpwpppk.k..",
  "...kkpwpppppk...",
  "..kpppppPppPpk..",
  "...kkppppPpPk...",
  "..k..kppppPk.k..",
  "......kkkkk.....",
  "................",
  "................",
  "................",
  "................",
};

static const char* const SPR_EGG[32] = {  // 32x32
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................k...............",
  ".............kkkykkk............",
  "............kyyyyyyyk...........",
  "...........kyyyyyyyyyk..........",
  "..........kyyyyyyyyyyyk.........",
  ".........kyyggyyyyyyyyyk........",
  ".........kyyggyyyyyyyyyk........",
  "........kyyygyyyyyyyyyyYk.......",
  "........kyyyyyyyyyyyyyYYk.......",
  "........kyyyyyyyyyyyyyYYk.......",
  "........kyyyyyyyyyyggyYYk.......",
  "........kyyyyyyyyyyggyYYk.......",
  "........kyyyyyyyyyyygyYYk.......",
  "........kyyyyyyyyyyyyyYYk.......",
  "........kyyyyyyyyyyyyyYYk.......",
  "........kyyyyyyyyyyyyYYYk.......",
  ".........kyyyggyyyyyyYYk........",
  ".........kyyyggyyyyyYYYk........",
  "..........kyyyyyyyYYYYk.........",
  "...........kyyYYYYYYYk..........",
  "............kYYYYYYYk...........",
  ".............kkkYkkk............",
  "................k...............",
  "................................",
  "................................",
  "................................",
};

static const char* const SPR_POOP[32] = {  // 32x32
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................k...............",
  ".................k..............",
  "...............kkk..............",
  "..............kccck.............",
  ".............kccccck............",
  "..............kccck.............",
  "..............kCCCk.............",
  "............kkccccckk...........",
  "............kccccccck...........",
  "...........kccccccccCk..........",
  "............kccccccck...........",
  "............kcccccCCk...........",
  "...........kccCCCCCcck..........",
  "..........kccccccccccck.........",
  "..........kccccccccccCk.........",
  ".........kcccccccccccCCk........",
  "..........kccccccccccCk.........",
  "..........kcccccccccCCk.........",
  "...........kkkcCCCCkkk..........",
  "..............kkkkk.............",
  "................................",
  "................................",
  "................................",
};

static const char* const SPR_HEART[32] = {  // 32x32
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "........krrrrk....krrrrk........",
  ".......krrrrrrk..krrrrrrk.......",
  "......krrrrrrrrrrrrrrrrrrk......",
  ".....krrrrrrrrrrrrrrrrrrrrk.....",
  ".....krrwwrrrrrrrrrrrrrrRrk.....",
  ".....krwwrrrrrrrrrrrrrrRRrk.....",
  "......krrrrrrrrrrrrrrrrrrk......",
  ".......krrrrrrrrrrrrrrrrk.......",
  "........krrrrrrrrrrrrrrk........",
  ".........krrrrrrrrrrrrk.........",
  "..........krrrrrrrrrrk..........",
  "...........krrrrrrrrk...........",
  "............krrrrrrk............",
  ".............krrrrk.............",
  "..............krrk..............",
  "...............kk...............",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
};

static const Species SPECIES[NUM_SPECIES] = {
  { nullptr, TYPE_FUEGO, SPR_FALLBACK[0], 5, SP_CHARMELEON, 16, 6, 9, 17, 12, 13, 0xF427, 0xEA87 },
  { nullptr, TYPE_FUEGO, SPR_FALLBACK[1], 6, SP_CHARIZARD, 36, 6, 8, 16, 12, 12, 0xEA87, 0xEA87 },
  { nullptr, TYPE_FUEGO, SPR_FALLBACK[2], 7, -1, 0, 6, 10, 18, 12, 14, 0xF427, 0xEA87 },
  { nullptr, TYPE_PLANTA, SPR_FALLBACK[3], 5, SP_IVYSAUR, 16, 10, 6, 13, 16, 9, 0x8EB6, 0x3C49 },
  { nullptr, TYPE_PLANTA, SPR_FALLBACK[4], 6, SP_VENUSAUR, 36, 10, 6, 13, 16, 9, 0x8EB6, 0x3C49 },
  { nullptr, TYPE_PLANTA, SPR_FALLBACK[5], 7, -1, 0, 10, 5, 12, 16, 8, 0x8EB6, 0x3C49 },
  { nullptr, TYPE_AGUA, SPR_FALLBACK[6], 5, SP_WARTORTLE, 16, 5, 8, 16, 11, 12, 0x7E3D, 0x4C98 },
  { nullptr, TYPE_AGUA, SPR_FALLBACK[7], 6, SP_BLASTOISE, 36, 6, 7, 15, 12, 11, 0x9D5C, 0x4C98 },
  { nullptr, TYPE_AGUA, SPR_FALLBACK[8], 7, -1, 0, 6, 11, 19, 12, 15, 0x3B74, 0x4C98 },
};

static const int8_t STARTERS[] = { SP_CHARMANDER, SP_BULBASAUR, SP_SQUIRTLE };
#define NUM_STARTERS 3
