/* TamaPoke UI port -- upstream TamaPoke.ino relaid from 466x466 circle to
 * 320x240 rectangle. Drawing API unchanged (tamapoke_gfx.h); every change
 * here is a coordinate change. Hit-testing bodies are upstream's, fed
 * synthesised coordinates by tamapoke_input.cpp. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tamapoke_gfx.h"
#include "tamapoke_input.h"
#include "tamapoke_ui.h"
#include "tamapoke_shim.h"
#include "Arduino.h"

#include "pet.h"
#include "dex.h"
#include "i18n.h"
#include "species.h"
#include "tamapoke_sprites.h"   /* PmdMon / PmdAct / thumbs / SPR_* / spriteColor */
#include "rtcbat.h"             /* batPercent / batCharging (user-pending) */
#include "audio.h"              /* forwarding shim -> tamapoke_audio.h */

#define C565(r, g, b) \
  ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

#define INK_K 0x18C4

enum {
  ACT_IDLE = 0, ACT_SLEEP, ACT_EAT, ACT_HURT,
  ACT_POSE, ACT_NOD, ACT_BREATH, ACT_COUNT
};

static const int16_t BTN_XS[4] = {BTN0_X, BTN1_X, BTN2_X, BTN3_X};

/* ---- globals ---- */

static Pet pet;
static PmdMon pmd;
static PmdMon evoPmd;
static PmdMon galleryPmd;
static int16_t monFor = -2;
static bool monShinyFor = false;

struct Beh {
  uint8_t mode = 0;
  uint8_t act = ACT_IDLE;
  uint32_t t0 = 0;
  uint32_t until = 0;
  float x = TP_CX;
  float targetX = TP_CX;
};
static Beh beh;

static bool galleryOpen, galleryDirty, cardOpen, kbOpen, clockOpen;
static bool gameOpen, sackOpen;
/* Harness override: >=0 forces the dispatcher to render the named screen
 * regardless of runtime state. Set by tamapoke_ui_goto_screen(); production
 * code never sets it. */
static int8_t harness_screen = -1;
static int galleryPage;
static int16_t galleryDetail;
static char nameBuf[12];
static uint8_t nameLen, cardPage;
static int clockH = 12, clockM = 0;
static uint32_t bathUntil, feedMenuUntil, confirmUntil, choiceUntil;
static uint32_t gameOverUntil, sackUntil, sackOverUntil, hitTime;
static bool bathPending, gameNewHi, sackNewHi, holdFired;
static uint8_t choiceKind, gameScore, gameMisses, sackGain;
static uint16_t sackHits;
static uint32_t gameStartedAt;
static int8_t paddleDir;   /* -1/0/+1, the direction currently held */
static float ballX, ballY, ballVX, ballVY, gamePetX, paddleX;
static float sackShake;
static uint32_t lastInteract, holdStart;
/* True if any input handler ran since the previous tick. Cleared at the top
 * of tamapoke_ui_tick; exposed via tamapoke_ui_had_activity(). */
/* How long a timed overlay stays up with no input. Named because each was
 * written twice as a literal, and because tamapoke_ui_note_input() has to renew
 * them with the same value it was opened with. */
#define FEED_MENU_MS 6000
#define CONFIRM_MS  10000
#define CHOICE_MS   15000

static bool g_had_activity = false;

static void note_activity(uint32_t now) {
  lastInteract = now;
  g_had_activity = true;
}
static uint8_t dimStage;
static bool gNight = false;

struct Bubble { int16_t x, y; uint8_t r, ph; };
static Bubble bubbles[14];

/* ---- focus sets ---- */

static const hitbox_t MAIN_BOXES[] = {
  {PET_ZONE_X, PET_ZONE_Y, PET_ZONE_W, PET_ZONE_H},
  {BTN0_X, BTN_ROW_Y, BTN_W, BTN_H},
  {BTN1_X, BTN_ROW_Y, BTN_W, BTN_H},
  {BTN2_X, BTN_ROW_Y, BTN_W, BTN_H},
  {BTN3_X, BTN_ROW_Y, BTN_W, BTN_H},
};
static const focus_set_t FOCUS_MAIN = {MAIN_BOXES, ARRAY_LEN(MAIN_BOXES), 0, 0};

static const hitbox_t CONFIRM_BOXES[] = {
  {CONFIRM_YES_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H},
  {CONFIRM_NO_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H},
};
static const focus_set_t FOCUS_CONFIRM = {CONFIRM_BOXES, ARRAY_LEN(CONFIRM_BOXES), 0, FOCUS_KIND_MODAL};

/* The evolve/farewell dialog's two rows.
 *
 * It had no focus set, so while it was up the D-pad walked the action buttons
 * underneath it and A -- which onTap() only accepts inside these two rectangles
 * while choiceUntil is running -- did nothing at all. The one decision in the game
 * that cannot be undone could not be taken with the buttons. Same shape as the
 * feed menu and the starter screen; third time. */
static const hitbox_t CHOICE_BOXES[] = {
  {EVO_BTN_X, EVO_BTN_Y, EVO_BTN_W, EVO_BTN_H},
  {FAR_BTN_X, FAR_BTN_Y, FAR_BTN_W, FAR_BTN_H},
};
static const focus_set_t FOCUS_CHOICE = {CHOICE_BOXES, ARRAY_LEN(CHOICE_BOXES), 0,
                                         FOCUS_KIND_MODAL};

static const focus_set_t FOCUS_GALLERY = {nullptr, 16, GAL_COLS, FOCUS_KIND_GALLERY};
static const focus_set_t FOCUS_KEYBOARD = {nullptr, 30, KB_COLS, FOCUS_KIND_KEYBOARD};

/* The three starter rows. This screen is the first thing a new save shows and
 * it had no focus set at all: tamapoke_current_focus_set() answered nullptr, so
 * nothing highlighted, A had no rectangle to tap, and the choice could not be
 * made -- the game could not be started at all with buttons. Same geometry the
 * onTap hit-test uses, so the two cannot disagree about where a row is. */
static const hitbox_t STARTER_BOXES[] = {
  {STARTER_ROW_X, STARTER_ROW_Y0, STARTER_ROW_W, STARTER_ROW_H},
  {STARTER_ROW_X, STARTER_ROW_Y0 + (STARTER_ROW_H + STARTER_ROW_GAP), STARTER_ROW_W, STARTER_ROW_H},
  {STARTER_ROW_X, STARTER_ROW_Y0 + 2 * (STARTER_ROW_H + STARTER_ROW_GAP), STARTER_ROW_W, STARTER_ROW_H},
};
static const focus_set_t FOCUS_STARTER = {STARTER_BOXES, ARRAY_LEN(STARTER_BOXES), 0, FOCUS_KIND_MODAL};

/* A screen that takes no directional focus still has to answer the question,
 * because the input layer walks whatever it is handed. Answering nullptr made
 * every button press on the starter/card/clock/game/sack screens dereference it:
 * focus_step() read fs->count through address 0, and the value it found in ITCM
 * was odd, so the following halfword load raised a UsageFault with
 * CFSR=0x01000000 (UNALIGNED) -- reported on device as a crash on any keypress.
 * An empty set is the honest answer: nothing to walk, nothing to tap, and B/back
 * and the swipes keep working because they never consult the set. */
static const focus_set_t FOCUS_NONE = {nullptr, 0, 0, 0};

/* The five feed cells. Without a set of its own the D-pad walked the main
 * screen's buttons underneath the open menu, so the menu could be opened and not
 * used -- the same shape as the starter screen and the settings screen. */
static const hitbox_t FEED_BOXES[] = {
  {FEED_ICON0_X + 0 * FEED_ICON_GAP, FEED_ICON_Y, FEED_ICON_SZ, FEED_ICON_SZ},
  {FEED_ICON0_X + 1 * FEED_ICON_GAP, FEED_ICON_Y, FEED_ICON_SZ, FEED_ICON_SZ},
  {FEED_ICON0_X + 2 * FEED_ICON_GAP, FEED_ICON_Y, FEED_ICON_SZ, FEED_ICON_SZ},
  {FEED_ICON0_X + 3 * FEED_ICON_GAP, FEED_ICON_Y, FEED_ICON_SZ, FEED_ICON_SZ},
  {FEED_ICON0_X + 4 * FEED_ICON_GAP, FEED_ICON_Y, FEED_ICON_SZ, FEED_ICON_SZ},
};
static const focus_set_t FOCUS_FEED = {FEED_BOXES, ARRAY_LEN(FEED_BOXES), 0, FOCUS_KIND_MODAL};

/* The clock/settings screen: hour -/+, minute -/+, the sound pill, the language
 * pill, and OK. It had no focus set at all -- it answered FOCUS_NONE -- so none
 * of it could be pressed with buttons. That is why the language could not be
 * changed to Korean on hardware: the pill was drawn, cycles all seven languages
 * when tapped, and there was no way to tap it. Same geometry as the onTap
 * hit-test above. */
static const hitbox_t CLOCK_BOXES[] = {
  {CLOCK_HMINUS_X, CLOCK_BTN_Y,  CLOCK_BTN_W,   CLOCK_BTN_H},
  {CLOCK_HPLUS_X,  CLOCK_BTN_Y,  CLOCK_BTN_W,   CLOCK_BTN_H},
  {CLOCK_MMINUS_X, CLOCK_BTN_Y,  CLOCK_BTN_W,   CLOCK_BTN_H},
  {CLOCK_MPLUS_X,  CLOCK_BTN_Y,  CLOCK_BTN_W,   CLOCK_BTN_H},
  {CLOCK_SOUND_X,  CLOCK_PILL_Y, CLOCK_SOUND_W, CLOCK_PILL_H},
  {CLOCK_LANG_X,   CLOCK_PILL_Y, CLOCK_LANG_W,  CLOCK_PILL_H},
  {CLOCK_OK_X,     CLOCK_OK_Y,   CLOCK_OK_W,    CLOCK_OK_H},
};
/* Card: rename (the name strip), TRAIN, and back. */
static const hitbox_t CARD_BOXES[] = {
  {0,            0,            GFX_WIDTH,    CARD_NAME_H},
  {CARD_TRAIN_X, CARD_TRAIN_Y, CARD_TRAIN_W, CARD_TRAIN_H},
  {TP_CX - CARD_BACK_W / 2, CARD_BACK_Y, CARD_BACK_W, CARD_BACK_H},
};
static const focus_set_t FOCUS_CARD = {CARD_BOXES, ARRAY_LEN(CARD_BOXES), 0, FOCUS_KIND_MODAL};

static const focus_set_t FOCUS_CLOCK = {CLOCK_BOXES, ARRAY_LEN(CLOCK_BOXES), 0, FOCUS_KIND_MODAL};

/* The egg: one target, the pet zone, so mashing A cracks it.
 *
 * FOCUS_KIND_LIST, like the main screen it is a state of -- so DOWN still opens the
 * Pokedex while the pet is unhatched. It was MODAL because an overrun used to fire
 * a swipe, and with one item every press was an overrun; overruns clamp now, so the
 * kind can say what this screen actually is. */
static const hitbox_t EGG_BOXES[] = {
  {PET_ZONE_X, PET_ZONE_Y, PET_ZONE_W, PET_ZONE_H},
};
static const focus_set_t FOCUS_EGG = {EGG_BOXES, ARRAY_LEN(EGG_BOXES), 0, FOCUS_KIND_LIST};

/* ---- helpers ---- */

/* Which screen the runtime state maps to. Defined next to the render dispatch it
 * feeds, and declared here because half the file needs the answer: the focus set,
 * the input mode and the key shortcuts all have to agree with what is on screen,
 * and each of them working it out separately is what let the renderer draw one
 * screen while the buttons drove another. */
static int current_screen_id();

static uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

static int sceneHour() {
  if (!tamapoke_clock_is_set()) return 13;
  return (tamapoke_epoch() / 3600) % 24;
}

static uint16_t lerp565(uint16_t a, uint16_t b, uint8_t i, uint8_t n) {
  if (n == 0) return a;
  uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint8_t r = ar + (uint16_t)(br - ar) * i / n;
  uint8_t g = ag + (uint16_t)(bg - ag) * i / n;
  uint8_t bl = ab + (uint16_t)(bb - ab) * i / n;
  return (r << 11) | (g << 5) | bl;
}

/* Width of a string when rendered through gfx->print().
 *
 * The firmware's print() switches wholesale: a string with ANY non-ASCII
 * byte is handed to tamapoke_draw_unicode and counts every UTF-8 lead byte
 * (Latin included) as one double-wide cell; only fully-ASCII strings take
 * the per-glyph Latin path that scales with textSize. So we have to mirror
 * that switch to land centring math in the same place the renderer does.
 * See tamapoke_gfx.cpp:print() and host_stubs.cpp:tamapoke_draw_unicode. */
static int textWidth(const char *s, uint8_t ts) {
  if (!s) return 0;
  bool has_wide = false;
  int cells = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if ((*p & 0xC0) != 0x80) cells++;
    if (*p & 0x80) has_wide = true;
  }
  /* Non-Latin is measured by the renderer that draws it, not guessed here --
   * the two disagreeing is how a centred label ends up off its button. */
  if (has_wide) return tamapoke_unicode_width(s);
  return cells * GFX_GLYPH_W * ts;
}

/* Height of a string as it will actually be drawn. Latin scales with the text
 * size; Hangul does not and is taller, so anything anchored to the bottom of
 * the panel must measure rather than assume. */
static int textHeight(const char *s, uint8_t ts) {
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++)
    if (*p & 0x80) return tamapoke_unicode_height();
  return GFX_GLYPH_H * ts;
}

static void centerText(const char *s, int16_t cx, int16_t y, uint8_t size);

/* Draw centred with the text sitting `margin` above the bottom edge. */
static void centerTextBottom(const char *s, int16_t cx, int16_t margin, uint8_t size) {
  centerText(s, cx, (int16_t)(GFX_HEIGHT - margin - textHeight(s, size)), size);
}

static void centerText(const char *s, int16_t cx, int16_t y, uint8_t size) {
  int16_t w = (int16_t)textWidth(s, size);
  gfx->setCursor(cx - w / 2, y);
  gfx->setTextSize(size);
  gfx->print(s);
}

/* `tint`, when non-zero, paints every opaque pixel that colour instead of the
 * one the map names. It exists so one berry drawing can serve the red, blue and
 * green berries rather than the tree carrying three near-identical maps. */
static void drawMap(const char *const *map, uint8_t n,
                    int16_t x, int16_t y, int16_t s, bool sil, uint16_t tint = 0) {
  for (uint8_t r = 0; r < n; r++) {
    const char *row = map[r];
    if (!row) break;
    for (uint8_t c = 0; c < n && row[c]; c++) {
      char ch = row[c];
      if (ch == '.') continue;
      uint16_t col = sil ? INK_K : (tint ? tint : spriteColor(ch));
      if (col == 0 && !sil) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, col);
    }
  }
}

static int8_t flashIdxForDex(int16_t dex) {
  switch (dex) {
    case 1: return SP_BULBASAUR;
    case 2: return SP_IVYSAUR;
    case 3: return SP_VENUSAUR;
    case 4: return SP_CHARMANDER;
    case 5: return SP_CHARMELEON;
    case 6: return SP_CHARIZARD;
    case 7: return SP_SQUIRTLE;
    case 8: return SP_WARTORTLE;
    case 9: return SP_BLASTOISE;
    default: return -1;
  }
}

static bool inPetZone(int16_t x, int16_t y) {
  return x >= PET_ZONE_X && x < PET_ZONE_X + PET_ZONE_W &&
         y >= PET_ZONE_Y && y < PET_ZONE_Y + PET_ZONE_H;
}

/* speciesId is 1..151; on a fresh Pet it is 0 until chooseStarter() runs, and
 * DEX_TBL[0] is not a real entry. Every read of DEX_TBL[pet.speciesId] goes
 * through here so a not-yet-started pet shows "?" instead of faulting. */
static const char *dexName(uint8_t sid) {
  return (sid >= 1 && sid <= DEX_COUNT) ? DEX_TBL[sid].name : "?";
}

static StrId statusMsg() {
  if (pet.evolving()) return S_EVOLVING;
  if (pet.eating()) return S_EATING;
  if (pet.showHeart()) return S_LIKES;
  PetMood m = pet.mood();
  if (m == MOOD_SLEEPING) return S_HAPPY;
  if (pet.lowestStat() < 25) {
    if (pet.fullness < 25) return S_HUNGRY;
    if (pet.hygiene < 25) return S_NEEDS_BATH;
    if (pet.energy < 25) return S_EXHAUSTED;
    return S_SAD;
  }
  if (pet.shiny) return S_IS_SHINY;
  return S_HAPPY;
}

static StrId eggMsg() {
  uint8_t rar = pet.eggRarity();
  if (rar == R_LEGENDARIO) return S_EGG_LEGEND;
  if (rar == R_RARO) return S_EGG_RARE;
  if (pet.eggCracks() >= 2) return S_EGG_ALMOST;
  if (pet.eggCracks() >= 1) return S_EGG_MOVES;
  return S_EGG_TOUCH;
}

/* ---- PMD helpers ---- */

static uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t total = 0;
  for (uint8_t f = 0; f < a.frames; f++) total += a.ms[f];
  return total;
}

static uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (total == 0) return 0;
  if (!loop) {
    if (t >= total) return a.frames - 1;
  } else {
    t %= total;
  }
  uint32_t acc = 0;
  for (uint8_t f = 0; f < a.frames; f++) {
    acc += a.ms[f];
    if (t < acc) return f;
  }
  return a.frames - 1;
}

static void drawPmdActM(const PmdMon &m, uint8_t actId, int16_t cx,
                        int16_t groundY, uint32_t t,
                        bool loop, bool sil, uint8_t maxS) {
  if (!m.loaded || actId >= ACT_COUNT) return;
  const PmdAct &a = m.acts[actId];
  if (a.frames == 0 || a.w == 0 || a.h == 0 || !a.data) return;

  uint8_t idleH = (m.has(PMD_IDLE) && m.acts[PMD_IDLE].h) ? m.acts[PMD_IDLE].h : a.h;
  uint8_t s = (uint8_t)constrain((int)(170 / idleH), 2, (int)maxS);
  if (a.w > 32 || a.h > 32) s = (s > 1) ? s - 1 : 1;

  uint8_t fi = pmdFrameAt(a, t, loop);
  int16_t x0 = cx - (a.w * s) / 2;
  int16_t y0 = groundY - a.base * s;

  const uint8_t *px = (const uint8_t *)a.data + (uint32_t)fi * a.w * a.h;
  for (uint8_t r = 0; r < a.h; r++) {
    for (uint8_t c = 0; c < a.w; c++) {
      uint8_t idx = px[r * a.w + c];
      /* tools/tamapoke/repack_tpk2.py writes TRANSPARENT = 0xFF, and index 0 is
       * an ordinary palette colour that nearest_index() hands out like any other.
       * This skipped 0 and drew 0xFF, so every transparent pixel came out as
       * pal[255] -- uninitialised, since packs carry far fewer than 256 entries --
       * which is the black rectangle around the pet on hardware, and every pixel
       * that legitimately used colour 0 was punched out of the sprite instead.
       * An index past the palette is treated as transparent rather than drawn
       * from uninitialised memory. */
      if (idx == PMD_TRANSPARENT_INDEX || idx >= m.palCount) continue;
      uint16_t col = sil ? INK_K : m.pal[idx];
      gfx->fillRect(x0 + c * s, y0 + r * s, s, s, col);
    }
  }
}

static void drawPmdAct(uint8_t actId, int16_t cx, int16_t groundY,
                       uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

/* ---- pet sprite management ---- */

static void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && sdReady) return;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  pmd.unload();
  beh.x = beh.targetX = TP_CX;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);
    /* No TPK1 fallback any more -- the sprite loader owns all three slots.
     * If pmd.load failed, drawPet() drops straight to the flash sprite. */
  }
}

static void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  if (r < 35) {
    beh.mode = 1;
    beh.act = ACT_IDLE;
    beh.targetX = constrain(beh.x + random(-40, 41), 90, 230);
    beh.until = now + 2000 + random(2000);
  } else if (r < 60) {
    beh.mode = 0;
    beh.act = ACT_POSE + random(3);
    beh.until = now + 800;
  } else {
    beh.mode = 0;
    beh.act = ACT_IDLE;
    beh.until = now + 2000 + random(3000);
  }
}

static void stepBeh() {
  uint32_t now = millis();
  if (now >= beh.until) behNext();
  if (beh.mode == 1) {
    float dx = beh.targetX - beh.x;
    if (fabsf(dx) > 1.0f) beh.x += dx * 0.03f;
    else { beh.x = beh.targetX; beh.mode = 0; }
  }
}

/* ---- scene ---- */

static void drawClouds(uint32_t now, uint16_t col) {
  for (uint8_t k = 0; k < 2; k++) {
    int16_t cx = (int16_t)(((now / 30) + k * 140) % 360) - 20;
    int16_t cy = 30 + k * 16;
    gfx->fillCircle(cx, cy, 8, col);
    gfx->fillCircle(cx + 8, cy + 2, 6, col);
    gfx->fillCircle(cx - 8, cy + 2, 6, col);
  }
}

static void drawScene(uint8_t biome, uint32_t now, bool night) {
  static const uint16_t SOIL[6] = {
    C565(0x7e, 0xc0, 0x7f), C565(0xdc, 0xca, 0x94),
    C565(0x4f, 0x8a, 0x55), C565(0x8a, 0x55, 0x44),
    C565(0xa8, 0x90, 0x6a), C565(0xe6, 0xee, 0xf5)
  };
  uint16_t soil = SOIL[biome % 6];
  int hour = sceneHour();
  bool isNight = night || (hour >= 20 || hour < 7);
  bool isDusk = (hour >= 17 && hour < 20);
  uint16_t skyTop, skyBot;

  if (isNight) {
    skyTop = C565(0x0c, 0x10, 0x24);
    skyBot = C565(0x1c, 0x20, 0x40);
  } else if (isDusk) {
    skyTop = C565(0x4a, 0x3a, 0x60);
    skyBot = C565(0xe8, 0x7a, 0x4a);
  } else {
    skyTop = C565(0x6c, 0xb2, 0xe4);
    skyBot = C565(0xc4, 0xe0, 0xf0);
  }

  for (int16_t y = 0; y < TP_HORIZON; y += 4) {
    uint16_t band = lerp565(skyTop, skyBot, y, TP_HORIZON);
    gfx->fillRect(0, y, GFX_WIDTH, 4, band);
  }

  if (isNight) {
    gfx->fillCircle(260, 30, 12, C565(0xd8, 0xdc, 0xf0));
    gfx->fillCircle(256, 26, 10, gNight ? UI_BG_NIGHT : skyTop);
    static const int16_t STARS[6][2] = {
      {103, 73}, {208, 63}, {228, 108}, {91, 118}, {183, 48}, {123, 51}
    };
    for (uint8_t i = 0; i < 6; i++)
      gfx->fillRect(STARS[i][0], STARS[i][1], 2, 2, UI_WHITE);
  } else if (isDusk) {
    gfx->fillCircle(TP_CX, 60, 10, C565(0xf8, 0xc0, 0x60));
  } else {
    gfx->fillCircle(250, 22, 7, C565(0xff, 0xf0, 0x60));
    drawClouds(now, UI_WHITE);
  }

  /* Down to the bottom of the screen, not to the pet's feet. The bottom panel is
   * translucent (see PANEL_ALPHA_DAY), so whatever is behind it has to be
   * repainted every frame or the blend accumulates towards solid white. It also
   * closes the 2px seam that used to sit between the old fill's end (150) and the
   * panel's start (152), which showed the fillScreen colour as a pale line. */
  gfx->fillRect(0, TP_HORIZON, GFX_WIDTH, GFX_HEIGHT - TP_HORIZON, soil);
  uint16_t hillCol = lerp565(soil, UI_WHITE, 3, 8);
  gfx->fillRoundRect(-20, TP_HORIZON - 14, 200, 30, 10, hillCol);
  if (biome == 2)
    gfx->fillRect(0, TP_HORIZON + 8, GFX_WIDTH, 6, C565(0x4c, 0x98, 0xc4));
}

/* ---- pet drawing ---- */

static void drawPetFlash(int8_t fi, bool silhouette) {
  if (fi < 0 || fi >= NUM_SPECIES) {
    centerText("?", TP_CX, TP_PET_CY - 16, 4);
    return;
  }
  const Species &sp = SPECIES[fi];
  uint8_t s = max(1, sp.scale / 2);
  int16_t sz = SPRITE_W * s;
  drawMap(sp.sprite, SPRITE_H, TP_CX - sz / 2, TP_PET_CY - sz / 2, s, silhouette);
}

static void drawPetPMD(uint32_t now) {
  if (pet.evolving()) return;
  uint8_t act = beh.act;
  bool loop = true;
  if (pet.mood() == MOOD_SLEEPING) { act = ACT_SLEEP; loop = true; }
  else if (pet.eating()) { act = ACT_EAT; loop = false; }
  else if (pet.mood() == MOOD_SAD) { act = ACT_HURT; loop = true; }
  drawPmdAct(act, (int16_t)beh.x, TP_PET_GROUND, now - beh.t0, loop, false, 5);
  if (pet.showHeart()) {
    drawMap(SPR_HEART, 16, (int16_t)beh.x + 8, TP_PET_CY - 16, 1, false);
  }
}

static void drawPet() {
  uint32_t now = millis();
  bool sil = pet.evolving();
  if (pmd.loaded) {
    drawPetPMD(now);
  } else {
    drawPetFlash(flashIdxForDex(pet.speciesId), sil);
  }
  if (tamapoke_current_focus_set() == &FOCUS_MAIN && tamapoke_input_focus() == 0) {
    uint16_t shadow = gNight ? C565(0x08, 0x08, 0x10) : C565(0x28, 0x28, 0x30);
    gfx->fillRect(PET_ZONE_X, TP_PET_GROUND - 3, PET_ZONE_W, 6, shadow);
  }
}

static void drawEgg() {
  int16_t s = 2;
  int16_t sz = SPRITE_W * s;
  drawMap(SPR_EGG, SPRITE_H, TP_CX - sz / 2, TP_PET_CY - sz / 2, s, false);
}

/* ---- header / battery / streak ---- */

static void drawBattery() {
  uint8_t pct = batPercent();
  bool charging = batCharging();
  uint16_t ink = inkColor();
  int16_t x = HDR_BATT_X, y = HDR_BATT_Y, w = HDR_BATT_W, h = HDR_BATT_H;
  gfx->drawRoundRect(x, y, w, h, 2, ink);
  gfx->fillRect(x + w, y + h / 2 - 2, 2, 4, ink);
  if (charging) {
    gfx->fillTriangle(x + w/2 - 2, y + 1, x + w/2 + 3, y + h/2,
                      x + w/2 - 1, y + h/2, ink);
    gfx->fillTriangle(x + w/2 + 2, y + h - 1, x + w/2 - 3, y + h/2,
                      x + w/2 + 1, y + h/2, ink);
  } else {
    int16_t fw = (w - 4) * pct / 100;
    uint16_t col = pct > 50 ? UI_BAR_OK : pct > 20 ? UI_BAR_WARN : UI_BAR_BAD;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
}

static void drawStreakBadge() {
  if (pet.streak == 0) return;
  uint16_t ink = inkColor();
  int16_t x = HDR_STREAK_X, y = HDR_STREAK_Y;
  gfx->fillTriangle(x + 4, y, x, y + 10, x + 8, y + 10, UI_BAR_WARN);
  gfx->fillTriangle(x + 4, y + 3, x + 2, y + 10, x + 6, y + 10, UI_BAR_BAD);
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", pet.streak);
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  gfx->setCursor(x + 11, y + 2);
  gfx->print(buf);
}

static void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();
  drawStreakBadge();
  gfx->setTextSize(HDR_NAME_SIZE);
  gfx->setTextColor(nameColor);
  gfx->setCursor(HDR_NAME_X, HDR_NAME_Y);
  gfx->print(name);
  gfx->setTextSize(1);
  gfx->setTextColor(inkColor());
  centerText(msg, TP_CX, HDR_NAME_Y + GFX_GLYPH_H + 2, 1);
}

/* ---- bars / buttons / poops ---- */

/* The frosted plate the bars and buttons sit on.
 *
 * Blended over the scene rather than filled: upstream's panel is translucent
 * white and this port had it as opaque #383844, which is not only a different
 * look but an unreadable one -- the labels below are drawn in inkColor(), dark in
 * the day theme, so they came out dark-on-dark. See PANEL_Y in tamapoke_ui.h.
 *
 * A rim along the top edge gives the plate an edge to be seen against; without it
 * a translucent panel over grass has no boundary at all. */
static void drawPanel() {
  const uint16_t glass = gNight ? UI_BG_NIGHT : UI_WHITE;
  const uint8_t alpha = gNight ? PANEL_ALPHA_NIGHT : PANEL_ALPHA_DAY;
  gfx->blendRect(0, PANEL_Y, GFX_WIDTH, PANEL_H, glass, alpha);
  gfx->blendRect(0, PANEL_Y, GFX_WIDTH, PANEL_RIM_H,
                 gNight ? UI_INK_NIGHT : UI_WHITE, 220);
}

/* A focus ring with square-cut corners.
 *
 * Drawn as a filled plate behind the widget, then the widget over it. The
 * obvious alternative -- N concentric drawRoundRect() calls at growing sizes --
 * keeps one radius for every ring, so each ring's corner arc lands somewhere
 * different and the outline comes out notched. That is what the starter rows and
 * the action buttons were doing, and what "the border looks odd" was about. */
static void drawFocusRing(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                          int16_t t, uint16_t color) {
  if (t <= 0) return;
  gfx->fillRoundRect(x - t, y - t, w + 2 * t, h + 2 * t, r + t, color);
}

/* A frosted card: drop shadow, translucent fill, bright rim.
 *
 * Everything that floats over the scene uses this -- the feed menu, the release
 * confirm, the evolve/farewell choice. They used to be flat opaque plates in
 * UI_BG_DAY with a hairline, which is a different material from the bottom panel
 * and from each other. One surface, three call sites. */
static void drawSurface(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r) {
  gfx->blendRoundRect(x + TP_CARD_SHADOW_OFF, y + TP_CARD_SHADOW_OFF, w, h, r,
                      INK_K, TP_CARD_SHADOW_A);
  gfx->blendRoundRect(x, y, w, h, r, gNight ? UI_BG_NIGHT : UI_WHITE,
                      TP_CARD_ALPHA);
  gfx->drawRoundRect(x, y, w, h, r, inkColor());
}

/* True for the saturated colours that carry meaning (yes/no, evolve/keep). Those
 * keep their fill and their white label; the neutral surfaces take ink. */
static bool isSemanticFill(uint16_t f) {
  return f == UI_BAR_OK || f == UI_BAR_WARN || f == UI_BAR_BAD;
}

/* One pressable tile, and the port's only way to say "selected".
 *
 * Focus is always the same three things: an accent plate behind the tile, an
 * accent surface, and a label that keeps its contrast. A tile whose colour means
 * something keeps that colour and takes the same plate, so focus stays one idea
 * without overwriting semantics.
 *
 * Returns the colour the caller must draw the label in -- the part every screen
 * previously worked out for itself, and the part the minigame got wrong (dark ink
 * on a dark playfield). */
static uint16_t drawTile(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                         bool focused, uint16_t fill) {
  const uint16_t ink = inkColor();
  if (focused) drawFocusRing(x, y, w, h, r, TP_FOCUS_RING, TP_ACCENT);
  const uint16_t surface = (focused && !isSemanticFill(fill)) ? TP_ACCENT : fill;
  gfx->fillRoundRect(x, y, w, h, r, surface);
  gfx->drawRoundRect(x, y, w, h, r, ink);
  return isSemanticFill(surface) ? UI_WHITE : ink;
}

/* A recessed well: the readouts and tracks that are not pressable (the name
 * preview, a progress track). Distinct from a tile on purpose -- if everything
 * looks pressable, nothing does. */
static void drawWell(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r) {
  gfx->fillRoundRect(x, y, w, h, r, UI_TRACK);
  gfx->drawRoundRect(x, y, w, h, r, inkColor());
}

/* Centre a label inside a tile, in the ink the tile asked for. */
static void tileLabel(const char *s, int16_t x, int16_t y, int16_t w, int16_t h,
                      uint8_t size, uint16_t ink) {
  gfx->setTextSize(size);
  gfx->setTextColor(ink);
  int16_t lw = (int16_t)textWidth(s, size);
  int16_t lh = (int16_t)textHeight(s, size);
  gfx->setCursor(x + w / 2 - lw / 2, y + (h - lh) / 2);
  gfx->print(s);
  gfx->setTextColor(inkColor());
}

/* A language-neutral key legend: the D-pad axis that does something on this
 * screen, and the button that acts. Pictograms rather than a string, because a
 * hint that has to be translated is seven new table rows (and StrId is indexed by
 * position, so it is also a permanent commitment) for something an arrow says
 * better. Pass key = 0 for the axis alone. */
static void drawKeyHintOn(int16_t cx, int16_t y, bool vertical, char key,
                          uint16_t ink, uint16_t bg) {
  const int16_t arrows_w = 24, key_w = key ? 22 : 0;
  int16_t x = cx - (arrows_w + key_w) / 2;

  for (uint8_t i = 0; i < 2; i++) {
    int16_t ax = x + i * 12 + 5, ay = y + 5;
    int16_t d = (i == 0) ? -5 : 5;
    if (vertical)
      gfx->fillTriangle(ax, ay + d, ax - 4, ay - d / 2, ax + 4, ay - d / 2, ink);
    else
      gfx->fillTriangle(ax + d, ay, ax - d / 2, ay - 4, ax - d / 2, ay + 4, ink);
  }
  if (!key) return;

  int16_t kx = x + arrows_w + 8;
  gfx->fillCircle(kx + 7, y + 5, 7, ink);
  gfx->setTextSize(1);
  gfx->setTextColor(bg);
  gfx->setCursor(kx + 3, y + 1);
  const char s[2] = {key, 0};
  gfx->print(s);
  gfx->setTextColor(inkColor());
}

/* The usual case: the current theme's ink on the current theme's background. */
static void drawKeyHint(int16_t cx, int16_t y, bool vertical, char key) {
  drawKeyHintOn(cx, y, vertical, key, inkColor(),
                gNight ? UI_BG_NIGHT : UI_BG_DAY);
}

static void drawBar(int16_t x, int16_t y, const char *label, uint8_t val) {
  uint16_t ink = inkColor();
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  gfx->setCursor(x, y + 2);
  gfx->print(label);
  int16_t bx = x + BAR_LABEL_GAP;
  uint16_t barCol = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, BAR_W, BAR_H, BAR_R, UI_TRACK);
  int16_t fw = (BAR_W - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, BAR_H - 4, BAR_R - 1, barCol);
  gfx->drawRoundRect(bx, y, BAR_W, BAR_H, BAR_R, ink);
}

static void drawBars() {
  drawBar(BAR_COL0_X, BAR_ROW0_Y, T(S_BAR_FOOD), pet.fullness);
  drawBar(BAR_COL1_X, BAR_ROW0_Y, T(S_BAR_JOY), pet.joy);
  drawBar(BAR_COL0_X, BAR_ROW1_Y, T(S_BAR_ENE), pet.energy);
  drawBar(BAR_COL1_X, BAR_ROW1_Y, T(S_BAR_HYG), pet.hygiene);
}

static void drawButtons() {
  static const char *const *ICONS[4] = {
    SPR_ICON_FOOD, SPR_ICON_PLAY, SPR_ICON_LIGHT, SPR_ICON_CLEAN
  };
  uint8_t f = tamapoke_input_focus();
  bool focused = (tamapoke_current_focus_set() == &FOCUS_MAIN && f >= 1 && f <= 4);
  /* Touch is gone -- the cursor IS the input, so the highlight has to be
   * unmissable. It used to pulse as well, which made this the only focus in the
   * port that moved; one accent plate on every screen reads better than four
   * different treatments, one of which breathes. */
  for (uint8_t i = 0; i < 4; i++) {
    int16_t bx = BTN_XS[i];
    bool sel = focused && (f - 1 == i);
    drawTile(bx, BTN_ROW_Y, BTN_W, BTN_H, TP_R_MD, sel,
             pet.sleeping ? UI_TRACK : UI_WHITE);
    drawMap(ICONS[i], 16, bx + BTN_HALF - 8, BTN_ROW_Y + BTN_HALF - 8, 1, false);
  }
}

static void drawPoops() {
  if (pet.isEgg() || pet.poops == 0) return;
  for (uint8_t i = 0; i < pet.poops && i < 3; i++)
    drawMap(SPR_POOP, 16, 80 + i * 30, TP_PET_GROUND - 8, 1, false);
}

/* ---- celebration banner ---- */

static void drawCelebration() {
  char buf[32];
  const char *txt = nullptr;
  if (pet.showMedal() && pet.newMedal) {
    for (uint8_t i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { txt = medalName(i); break; }
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    txt = buf;
  }
  if (!txt) return;
  int16_t bw = (int16_t)textWidth(txt, 2) + 16;
  int16_t bx = TP_CX - bw / 2;
  int16_t by = 30;
  uint16_t lab = drawTile(bx, by, bw, 22, TP_R_MD, false, TP_ACCENT);
  tileLabel(txt, bx, by, bw, 22, 2, lab);
}

/* ---- CTA buttons ---- */

/* One CTA left. drawEvolveButton()/drawFarewellButton() lived here too, each with
 * its own pulse, radius and border colour -- both dead since the choice dialog
 * took over the same two decisions. Two ways to draw one button is how a screen
 * ends up looking like two screens. */
static void drawRunawayButton(uint32_t now) {
  (void)now;
  uint16_t lab = drawTile(RUN_BTN_X, RUN_BTN_Y, RUN_BTN_W, RUN_BTN_H, TP_R_MD,
                          false, C565(0x3a, 0x44, 0x5a));
  tileLabel(T(S_RUNAWAY_BTN), RUN_BTN_X, RUN_BTN_Y, RUN_BTN_W, RUN_BTN_H, 1,
            UI_WHITE);
  (void)lab;
}

/* ---- choice dialog ---- */

static void drawChoiceDialog(uint32_t now) {
  (void)now;
  drawSurface(CHOICE_DIALOG_X, CHOICE_DIALOG_Y, CHOICE_DIALOG_W, CHOICE_DIALOG_H,
              TP_R_LG);
  const char *q = (choiceKind == 1) ? T(S_EVO_Q) : T(S_FAR_Q);
  centerText(q, TP_CX, CHOICE_DIALOG_Y + 10, 1);

  /* Both rows are focusable, and the dialog never showed which one A would take.
   * FOCUS_CONFIRM's geometry is the confirm dialog's, so this uses its own. */
  uint8_t f = (tamapoke_current_focus_set() == &FOCUS_CHOICE)
              ? tamapoke_input_focus() : 0xFF;

  uint16_t actCol = (choiceKind == 1) ? UI_BAR_BAD : UI_BAR_WARN;
  uint16_t lab = drawTile(EVO_BTN_X, EVO_BTN_Y, EVO_BTN_W, EVO_BTN_H, TP_R_MD,
                          f == 0, actCol);
  tileLabel((choiceKind == 1) ? T(S_EVO_TAP) : T(S_FAR_GO),
            EVO_BTN_X, EVO_BTN_Y, EVO_BTN_W, EVO_BTN_H, 1, lab);

  uint16_t keepCol = (choiceKind == 1) ? UI_WHITE : UI_BAR_OK;
  lab = drawTile(FAR_BTN_X, FAR_BTN_Y, FAR_BTN_W, FAR_BTN_H, TP_R_MD,
                 f == 1, keepCol);
  tileLabel((choiceKind == 1) ? T(S_EVO_KEEP) : T(S_FAR_STAY),
            FAR_BTN_X, FAR_BTN_Y, FAR_BTN_W, FAR_BTN_H, 1, lab);
}

/* ---- evolve FX ---- */

static void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();
  int16_t cx = TP_CX, cy = TP_PET_CY;
  uint16_t halo = lerp565(UI_WHITE, UI_BAR_OK, (uint8_t)(t * 255), 255);
  int16_t r = (int16_t)(20 + t * 40);
  for (uint8_t i = 0; i < 8; i++) {
    float ang = t * 6.28f + i * 0.785f;
    int16_t rx = cx + (int16_t)(cosf(ang) * r);
    int16_t ry = cy + (int16_t)(sinf(ang) * r);
    gfx->fillCircle(rx, ry, 3, halo);
  }
  gfx->fillCircle(cx, cy, r / 2, halo);
  bool sil = ((int)(t * 10) % 2) == 0;
  if (pmd.loaded)
    drawPmdAct(beh.act, (int16_t)beh.x, TP_PET_GROUND, now - beh.t0, true, sil, 5);
  if (t > 0.9f) {
    gfx->fillRect(0, 0, GFX_WIDTH, GFX_HEIGHT, UI_WHITE);
  }
}

/* ---- ceremony ---- */

static void drawCeremony() {
  float t = pet.ceremonyT();
  uint32_t now = millis();
  if (pet.ceremony == CER_FAREWELL) {
    drawScene(0, now, false);
    int16_t r = (int16_t)(30 + sinf(now * 0.003f) * 5);
    gfx->fillCircle(TP_CX, TP_PET_CY, r, C565(0xff, 0xd0, 0x40));
    for (uint8_t i = 0; i < 6; i++) {
      float ang = t * 3.14f + i * 1.05f;
      int16_t hx = TP_CX + (int16_t)(cosf(ang) * 40);
      int16_t hy = TP_PET_CY + (int16_t)(sinf(ang) * 30);
      drawMap(SPR_HEART, 16, hx - 8, hy - 8, 1, false);
    }
  } else if (pet.ceremony == CER_RUNAWAY) {
    drawScene(0, now, true);
    for (uint8_t i = 0; i < 20; i++) {
      int16_t rx = (int16_t)((i * 37 + now / 50) % GFX_WIDTH);
      int16_t ry = (int16_t)((i * 53 + now / 30) % GFX_HEIGHT);
      gfx->drawLine(rx, ry, rx, ry + 4, C565(0x4c, 0x98, 0xc4));
    }
    int16_t walkX = TP_CX - (int16_t)(t * 200);
    if (pmd.loaded)
      drawPmdAct(ACT_IDLE, walkX, TP_PET_GROUND, now, true, true, 5);
  }
  if (t > 0.95f) pet.newEgg();
}

/* ---- bath ---- */

/* Screen openers, ported from upstream's TamaPoke.ino. The port had the renderers
 * for all six sub-screens and no way to reach any of them: the only code that set
 * cardOpen / galleryOpen / kbOpen / clockOpen / gameOpen / sackOpen was
 * tamapoke_ui_goto_screen(), which exists for the host harness. So the harness drew
 * them all and the device could not open one -- the settings screen included, which
 * is why its language pill was unreachable. */
static void openClock(void) {
  uint32_t e = pet.lastSeenEpoch ? pet.lastSeenEpoch : tamapoke_epoch();
  clockH = (uint8_t)((e / 3600) % 24);
  clockM = (uint8_t)((e / 60) % 60);
  clockOpen = true;
  tamapoke_input_reset(0);
}

static void openKeyboard(void) {
  kbOpen = true;
  strncpy(nameBuf, pet.nick, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = '\0';
  nameLen = (uint8_t)strlen(nameBuf);
  tamapoke_input_reset(0);
}

static void startGame(void) {
  if (pet.isEgg() || pet.sleeping || pet.ceremony != CER_NONE) return;
  gameOpen = true;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameStartedAt = millis();
  paddleDir = 0;
  ballX = GFX_WIDTH / 2;
  ballY = GAME_TOP + 30;
  ballVX = 1.5f;
  ballVY = 1.0f;
  paddleX = (GFX_WIDTH - GAME_PADDLE_W) / 2;
  tamapoke_input_reset(0);
}

static void startSack(void) {
  if (pet.isEgg() || pet.sleeping || pet.ceremony != CER_NONE) return;
  sackOpen = true;
  sackUntil = millis() + SACK_ROUND_MS;
  sackOverUntil = 0;
  sackHits = 0;
  sackGain = 0;
  sackNewHi = false;
  sackShake = 0;
  tamapoke_input_reset(0);
}

/* ---- minigame input, and the endings neither game had ----
 *
 * Both minigames shipped as animations. The ball bounced off a paddle nothing
 * could move (paddleX was written once, in startGame, and never again), the sack
 * hung there while a timer ran down, and sackHits was never incremented by
 * anything -- so pet.playResult() and pet.trainStrength(), the two functions that
 * turn a round into training, were dead code in the tree. Neither game could be
 * played and neither could be won. */

tp_input_mode_t tamapoke_ui_input_mode(void) {
  switch (current_screen_id()) {
    case 6: return TP_INPUT_PADDLE;
    case 7: return TP_INPUT_MASH;
    default: return TP_INPUT_UI;
  }
}

void tamapoke_paddle_hold(int dir) {
  paddleDir = (int8_t)(dir < 0 ? -1 : (dir > 0 ? 1 : 0));
  if (dir) note_activity(millis());
}

void tamapoke_sack_hit(void) {
  if (!sackOpen) return;
  uint32_t now = millis();
  if (now >= sackUntil) return;          /* the round is over; the result is up */
  note_activity(now);
  dimStage = 0;
  if (sackHits < 0xFFFF) sackHits++;
  sackShake = SACK_SHAKE_PX;
  hitTime = now;
  sfxPlay(SFX_TAP);
}

/* Close out a ball round: reward it, remember the record, show the result. */
static void endGame(void) {
  gameNewHi = (gameScore > pet.gameHi);
  pet.playResult(gameScore);             /* trains SPE, burns weight, adds joy */
  gameOverUntil = millis() + GAME_OVER_MS;
  sfxPlay(gameNewHi ? SFX_MEDAL : SFX_PLAY);
}

/* Same for a training round. */
static void endSack(void) {
  sackNewHi = (sackHits > pet.strHi);
  sackGain = pet.trainStrength(sackHits); /* trains ATK; returns the gain */
  sackOverUntil = sackUntil + SACK_RESULT_MS;
  sfxPlay(sackNewHi ? SFX_MEDAL : SFX_LEVEL);
}

static void startBath(uint32_t now) {
  bathUntil = now + 3000;
  bathPending = true;
  for (uint8_t i = 0; i < 14; i++) {
    bubbles[i].x = random(PET_ZONE_X, PET_ZONE_X + PET_ZONE_W);
    bubbles[i].y = random(TP_PET_CY, TP_PET_GROUND);
    bubbles[i].r = random(3, 8);
    bubbles[i].ph = random(0, 64);
  }
}

static void drawBath(uint32_t now) {
  if (now >= bathUntil) {
    if (bathPending) { pet.clean(); bathPending = false; }
    return;
  }
  uint32_t elapsed = bathUntil - now;
  if (elapsed > 800) {
    for (uint8_t i = 0; i < 14; i++) {
      int16_t bx = bubbles[i].x;
      int16_t by = bubbles[i].y - (int16_t)(sinf((now + bubbles[i].ph * 100) * 0.003f) * 4);
      gfx->fillCircle(bx, by, bubbles[i].r, UI_WHITE);
      gfx->drawCircle(bx, by, bubbles[i].r, C565(0x4c, 0x98, 0xc4));
    }
  } else {
    for (uint8_t i = 0; i < 8; i++) {
      float ang = i * 0.785f;
      int16_t sx = TP_CX + (int16_t)(cosf(ang) * 20);
      int16_t sy = TP_PET_CY + (int16_t)(sinf(ang) * 20);
      gfx->fillCircle(sx, sy, 3, UI_WHITE);
    }
  }
}

/* ---- feed menu ---- */

static void drawFeedMenu(uint32_t now) {
  (void)now;
  drawSurface(FEED_MENU_X, FEED_MENU_Y, FEED_MENU_W, FEED_MENU_H, FEED_MENU_R);

  /* This used to fill each cell with UI_BAR_OK / UI_BAR_WARN alternately and
   * never draw anything else -- it even built an ICONS[] table and did not read
   * it. On hardware that is a row of plain green and orange squares with no way
   * to tell what any of them feeds, which is what was reported. The order has to
   * match onTap's: 0 = a meal, 1..3 = the red / blue / green berry, 4 = candy. */
  struct Item { const char *const *map; uint16_t tint; };
  const Item items[5] = {
    {SPR_ICON_FOOD,    0},              /* the apple keeps its own colours */
    {SPR_ICON_BERRY_G, UI_BERRY_RED},   /* red   */
    {SPR_ICON_BERRY_B, 0},              /* blue  */
    {SPR_ICON_BERRY_G, 0},              /* green */
    {SPR_ICON_CANDY,   0},
  };

  uint8_t f = (tamapoke_current_focus_set() == &FOCUS_FEED)
              ? tamapoke_input_focus() : 0xFF;

  for (uint8_t i = 0; i < 5; i++) {
    int16_t ix = FEED_ICON0_X + i * FEED_ICON_GAP;
    bool sel = (f == i);

    drawTile(ix, FEED_ICON_Y, FEED_ICON_SZ, FEED_ICON_SZ, TP_R_SM, sel, UI_WHITE);

    /* The maps are 16x16 inside a 24px cell. */
    drawMap(items[i].map, 16, ix + (FEED_ICON_SZ - 16) / 2,
            FEED_ICON_Y + (FEED_ICON_SZ - 16) / 2, 1, false, items[i].tint);
  }
}

/* ---- release confirm ---- */

static void drawConfirmDialog() {
  drawSurface(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, TP_R_LG);
  char buf[48];
  snprintf(buf, sizeof(buf), T(S_RELEASE_FMT),
           pet.nick[0] ? pet.nick : dexName(pet.speciesId));
  centerText(buf, TP_CX, CONFIRM_Y + 12, 1);

  uint8_t f = (tamapoke_current_focus_set() == &FOCUS_CONFIRM)
              ? tamapoke_input_focus() : 0xFF;
  /* Red stays red and green stays green whether focused or not -- the accent
   * plate says which one A takes, so focus does not have to repaint the meaning.
   * (The old version swapped fill and text colours on focus, and drew the YES
   * label twice, the first time in the wrong colour.) */
  uint16_t lab = drawTile(CONFIRM_YES_X, CONFIRM_BTN_Y, CONFIRM_BTN_W,
                          CONFIRM_BTN_H, TP_R_SM, f == 0, UI_BAR_BAD);
  tileLabel(T(S_YES), CONFIRM_YES_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H,
            1, lab);
  lab = drawTile(CONFIRM_NO_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H,
                 TP_R_SM, f == 1, UI_BAR_OK);
  tileLabel(T(S_NO), CONFIRM_NO_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H,
            1, lab);
}

/* ---- input handlers ---- */

void onTap(int16_t x, int16_t y) {
  uint32_t now = millis();
  note_activity(now);
  dimStage = 0;

  if (kbOpen) {
    if (y < KB_Y) { kbOpen = false; pet.rename(nameBuf); return; }
    for (uint8_t r = 0; r < KB_ROWS; r++)
      for (uint8_t c = 0; c < KB_COLS; c++) {
        uint8_t idx = r * KB_COLS + c;
        if (idx >= 30) break;
        int16_t kx = KB_X + c * KB_W, ky = KB_Y + r * KB_H;
        if (x >= kx && x < kx + KB_W && y >= ky && y < ky + KB_H) {
          if (idx == KB_SPECIAL0) {
            if (nameLen > 0) nameBuf[--nameLen] = 0;
          } else if (idx == KB_SPECIAL1) {
            kbOpen = false;
            pet.rename(nameBuf);
          } else if (nameLen < 11) {
            nameBuf[nameLen++] = KB_KEYS[idx];
            nameBuf[nameLen] = 0;
          }
          return;
        }
      }
    return;
  }
  if (galleryOpen) {
    /* Any tap on the detail view returns to the grid. */
    if (galleryDetail > 0) {
      galleryDetail = 0;
      galleryPmd.unload();
      return;
    }
    if (y < GAL_Y) { galleryOpen = false; galleryPmd.unload(); return; }
    for (uint8_t r = 0; r < GAL_ROWS; r++)
      for (uint8_t c = 0; c < GAL_COLS; c++) {
        int16_t gx = GAL_X + c * GAL_CELL, gy = GAL_Y + r * GAL_CELL;
        if (x >= gx && x < gx + GAL_CELL && y >= gy && y < gy + GAL_CELL) {
          int16_t dex = galleryPage * 16 + r * GAL_COLS + c + 1;
          if (dex <= DEX_COUNT) {
            galleryDetail = dex;
            galleryPmd.load(dex, pet.isShinyRegistered(dex));
          }
          return;
        }
      }
    return;
  }
  if (clockOpen) {
    if (y >= CLOCK_BTN_Y && y < CLOCK_BTN_Y + CLOCK_BTN_H) {
      if (x >= CLOCK_HMINUS_X && x < CLOCK_HMINUS_X + CLOCK_BTN_W)
        { clockH = (clockH + 23) % 24; return; }
      if (x >= CLOCK_HPLUS_X && x < CLOCK_HPLUS_X + CLOCK_BTN_W)
        { clockH = (clockH + 1) % 24; return; }
      if (x >= CLOCK_MMINUS_X && x < CLOCK_MMINUS_X + CLOCK_BTN_W)
        { clockM = (clockM + 59) % 60; return; }
      if (x >= CLOCK_MPLUS_X && x < CLOCK_MPLUS_X + CLOCK_BTN_W)
        { clockM = (clockM + 1) % 60; return; }
    }
    if (y >= CLOCK_PILL_Y && y < CLOCK_PILL_Y + CLOCK_PILL_H) {
      if (x >= CLOCK_SOUND_X && x < CLOCK_SOUND_X + CLOCK_SOUND_W)
        { audioSetEnabled(!audioEnabled()); return; }
      if (x >= CLOCK_LANG_X && x < CLOCK_LANG_X + CLOCK_LANG_W)
        /* setLang(), not `gLang = ...`: assigning the variable switched the UI
         * but skipped both of setLang()'s other jobs -- persisting the choice
         * (so it reset to English on every launch) and repointing DEX_TBL at
         * the new language's names on the card (so species kept the previous
         * language's names). Upstream's own setter does all three. */
        { setLang((Lang)((gLang + 1) % LANG_COUNT)); return; }
    }
    if (y >= CLOCK_OK_Y && y < CLOCK_OK_Y + CLOCK_OK_H &&
        x >= CLOCK_OK_X && x < CLOCK_OK_X + CLOCK_OK_W) {
      uint32_t epoch = (uint32_t)clockH * 3600 + (uint32_t)clockM * 60;
      epoch += tamapoke_epoch() / 86400 * 86400;
      pet.setClock(epoch);
      clockOpen = false; return;
    }
  }
  if (cardOpen) {
    /* Upstream renames from a tap on the name and starts the training sack from a
     * button on the card's second page. Our card is one page, so both live here. */
    if (y < CARD_NAME_H) { cardOpen = false; openKeyboard(); return; }
    if (y >= CARD_TRAIN_Y && y < CARD_TRAIN_Y + CARD_TRAIN_H &&
        x >= CARD_TRAIN_X && x < CARD_TRAIN_X + CARD_TRAIN_W) {
      cardOpen = false;
      startSack();
      return;
    }
    cardOpen = false;
    return;
  }
  if (gameOpen) { gameOpen = false; return; }
  if (sackOpen) { sackOpen = false; return; }
  if (pet.awaitingStarter()) {
    for (uint8_t i = 0; i < 3; i++) {
      int16_t ry = STARTER_ROW_Y0 + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (y >= ry && y < ry + STARTER_ROW_H) {
        pet.chooseStarter(CLASSIC_DEX[i]);
        return;
      }
    }
    return;
  }

  if (now < choiceUntil) {
    if (x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
        y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
      if (choiceKind == 1) pet.evolve(); else pet.startFarewell();
      choiceUntil = 0;
      return;
    }
    if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
        y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
      if (choiceKind == 1) pet.declineEvolve(); else pet.declineFarewell();
      choiceUntil = 0;
      return;
    }
    return;
  }

  if (now < confirmUntil) {
    if (y >= CONFIRM_BTN_Y && y <= CONFIRM_BTN_Y + CONFIRM_BTN_H) {
      if (x >= CONFIRM_YES_X && x < CONFIRM_YES_X + CONFIRM_BTN_W) pet.release();
      confirmUntil = 0;
      return;
    }
    return;
  }

  if (now < feedMenuUntil) {
    for (uint8_t i = 0; i < 5; i++) {
      int16_t ix = FEED_ICON0_X + i * FEED_ICON_GAP;
      if (x >= ix && x < ix + FEED_ICON_SZ && y >= FEED_ICON_Y &&
          y < FEED_ICON_Y + FEED_ICON_SZ) {
        if (i == 0) pet.feed();
        else if (i < 4) pet.feedBerry(i - 1);
        else pet.feedCandy();
        sfxPlay(SFX_EAT);
        feedMenuUntil = 0;
        return;
      }
    }
    feedMenuUntil = 0;
    return;
  }

  for (uint8_t i = 0; i < 4; i++) {
    int16_t bx = BTN_XS[i];
    if (x >= bx && x < bx + BTN_W && y >= BTN_ROW_Y && y < BTN_ROW_Y + BTN_H) {
      switch (i) {
        case 0: feedMenuUntil = now + FEED_MENU_MS; break;
        case 1: startGame(); break;   /* upstream: the ball minigame */
        case 2: pet.toggleLight(); break;
        case 3: startBath(now); sfxPlay(SFX_TAP); break;
      }
      return;
    }
  }

  if (pet.isEgg() && inPetZone(x, y)) { pet.eggTap(); sfxPlay(SFX_HEART); return; }
  if (!pet.isEgg() && inPetZone(x, y)) { pet.caress(); sfxPlay(SFX_HEART); return; }
}

void onSwipe(int dir) {
  /* Horizontal: upstream opens the gallery from the main screen and pages it once
   * open. Our card is a single page, so a horizontal gesture there just closes it
   * rather than turning to a page that does not exist. */
  if (pet.awaitingStarter()) return;
  if (gameOpen || kbOpen || clockOpen || sackOpen) return;
  note_activity(millis());
  dimStage = 0;
  if (cardOpen) { cardOpen = false; return; }
  if (!galleryOpen) {
    if (pet.ceremony == CER_NONE && millis() >= confirmUntil) {
      galleryOpen = true;
      galleryPage = 0;
      galleryDetail = 0;
      tamapoke_input_reset(0);
    }
    return;
  }
  if (dir > 0) { if (galleryPage > 0) galleryPage--; }
  else if (galleryPage < (DEX_COUNT - 1) / 16) galleryPage++;
}

void onSwipeV(int dir) {
  /* Vertical: down opens the clock/settings, up opens the status card. Both close
   * on the opposite gesture. Ported from upstream; our version had been reduced to
   * paging the gallery, which left every screen below unreachable. */
  if (pet.awaitingStarter()) return;
  if (gameOpen || kbOpen || sackOpen || pet.ceremony != CER_NONE) return;
  note_activity(millis());
  dimStage = 0;
  if (galleryOpen) {
    if (galleryPage < (DEX_COUNT - 1) / 16) galleryPage++;
    return;
  }
  if (clockOpen) { clockOpen = false; return; }
  if (cardOpen)  { if (dir < 0) cardOpen = false; return; }
  uint32_t now = millis();
  if (now < confirmUntil || now < feedMenuUntil) return;
  if (dir > 0) {
    openClock();
  } else if (!pet.isEgg()) {
    cardOpen = true;
    cardPage = 0;
    tamapoke_input_reset(0);
  }
}

void onBack(void) {
  uint32_t now = millis();
  note_activity(now);
  dimStage = 0;
  if (kbOpen) { kbOpen = false; pet.rename(nameBuf); return; }
  if (galleryOpen && galleryDetail > 0) {
    /* Out of the detail view, not out of the Pokedex: B used to close the whole
     * screen from inside a species page, so backing out cost you your place. */
    galleryDetail = 0;
    galleryPmd.unload();
    return;
  }
  if (cardOpen) { cardOpen = false; return; }
  if (galleryOpen) { galleryOpen = false; return; }
  if (clockOpen) { clockOpen = false; return; }
  if (gameOpen) { gameOpen = false; gameOverUntil = 0; return; }
  if (sackOpen) { sackOpen = false; sackOverUntil = 0; return; }
  /* Nothing open: B on the main screen dismisses whatever overlay is up, which is
   * the same "cancel" the dialogs get. */
  if (now < feedMenuUntil) { feedMenuUntil = 0; return; }
  if (now < confirmUntil) { confirmUntil = 0; return; }
}

/* ---- the two labelled keys, and the two vertical shortcuts ----
 *
 * Reaching a screen by walking the cursor off the edge of a row worked and was
 * not discoverable: the Pokedex was six presses of RIGHT until you fell off the
 * end of the button row, and it also fired every time someone overshot the last
 * button. TIME and GAME are printed on the console next to the screen, so they
 * take the two screens they name, and UP/DOWN take the other two. Nothing has to
 * be walked off any more. */

/* True when the main screen is showing and no overlay owns the input. */
static bool mainScreenIdle(void) {
  if (current_screen_id() != 0) return false;
  if (pet.ceremony != CER_NONE || pet.evolving()) return false;
  uint32_t now = millis();
  return now >= confirmUntil && now >= choiceUntil && now >= feedMenuUntil;
}

void tamapoke_time_key(void) {
  note_activity(millis());
  dimStage = 0;
  if (clockOpen) { clockOpen = false; return; }   /* toggles */
  if (!mainScreenIdle()) return;
  openClock();
}

void tamapoke_game_key(void) {
  note_activity(millis());
  dimStage = 0;
  if (gameOpen) { gameOpen = false; gameOverUntil = 0; return; }
  if (!mainScreenIdle()) return;
  startGame();
}

void tamapoke_open_card(void) {
  if (!mainScreenIdle() || pet.isEgg()) return;
  note_activity(millis());
  dimStage = 0;
  cardOpen = true;
  cardPage = 0;
  tamapoke_input_reset(0);
}

void tamapoke_open_gallery(void) {
  if (!mainScreenIdle()) return;
  note_activity(millis());
  dimStage = 0;
  galleryOpen = true;
  galleryPage = 0;
  galleryDetail = 0;
  tamapoke_input_reset(0);
}

void tamapoke_ui_probe(tamapoke_probe_t *out) {
  if (!out) return;
  out->screen = current_screen_id();
  out->paddle_x = (int)paddleX;
  out->ball_x = (int)ballX;
  out->ball_y = (int)ballY;
  out->game_score = gameScore;
  out->game_misses = gameMisses;
  out->sack_hits = sackHits;
  out->sack_gain = sackGain;
  out->pet_tr_atk = pet.trAtk;
  out->pet_tr_spe = pet.trSpe;
}

/* ---- focus / dim / hold ---- */

/* Defined further down, next to the render dispatch it feeds. Declared here so
 * the focus set can be derived from it rather than re-deriving the screen -- the
 * two answering separately is what let the renderer draw one screen while the
 * buttons drove another. */
static int current_screen_id();

/* Set when the canvas has been overwritten by something that is not a screen --
 * the launcher's pause menu, which paints wherever it likes. The next render
 * then repaints in full instead of drawing incrementally onto someone else's
 * pixels, which is what left the band under the egg black on return. */
static bool g_force_full_repaint = false;

void tamapoke_ui_force_full_repaint(void) { g_force_full_repaint = true; }

/* Push back whichever timed overlay is currently up.
 *
 * Upstream's timeouts are tuned for a finger: tap the feed button, tap the food.
 * Three seconds is plenty for that and not enough for a cursor, which has to be
 * WALKED to the item first -- so the feed menu closed under the player and the
 * food could not be chosen at all. The confirm and choice dialogs have the same
 * shape with more slack.
 *
 * Rather than pick bigger numbers and hope, any button press while one of these
 * is open restores its full duration: the menu closes when you stop interacting,
 * which is what a menu should do, and the touch path is unchanged because a tap
 * that hits an item dismisses it anyway. */
/* Identity of the feed focus set, for the harness: asserting "the feed menu is
 * still open" by pointer is exact, where re-deriving the condition in the test
 * would just restate the code under test. */
const focus_set_t *tamapoke_focus_set_feed(void) { return &FOCUS_FEED; }

/* Harness support. goto_screen() pins harness_screen so a screen can be captured
 * regardless of state; releasing it hands the decision back to the runtime, which
 * is the only way a test can ask "can the buttons get here from the main screen?".
 * Without that, every navigation check would be measuring the pin. */
void tamapoke_ui_release_harness_screen(void) { harness_screen = -1; }

int tamapoke_ui_current_screen(void) { return current_screen_id(); }

void tamapoke_ui_note_input(void) {
  uint32_t now = millis();
  if (now < feedMenuUntil) feedMenuUntil = now + FEED_MENU_MS;
  if (now < confirmUntil)  confirmUntil  = now + CONFIRM_MS;
  if (now < choiceUntil)   choiceUntil   = now + CHOICE_MS;
}

/* Derived from current_screen_id(), the same function the renderer switches on,
 * because the two answering independently is a bug generator: whatever is drawn
 * is what the buttons must drive, and any divergence shows up as a screen you
 * can see and cannot use.
 *
 * It had diverged twice in opposite directions. First the input side answered
 * nullptr for five screens while the renderer happily drew them (a crash on any
 * keypress). Then a reordering here -- meant to stop the minigame being driven by
 * the starter rows underneath it -- put awaitingStarter() BELOW the open-screen
 * flags, while the renderer still checks it first: the starter screen was drawn
 * with FOCUS_NONE behind it, so it appeared and did nothing. Both are impossible
 * now, since there is only one dispatch. */
const focus_set_t *tamapoke_current_focus_set(void) {
  switch (current_screen_id()) {
    case 5: return &FOCUS_STARTER;
    case 2: return &FOCUS_GALLERY;
    case 3: return &FOCUS_KEYBOARD;
    /* Card, clock, minigame and sack are dismissed with B or a swipe and take no
     * directional focus. They still hand back a set -- never nullptr -- because
     * the input layer walks whatever it is given. */
    case 4: return &FOCUS_CLOCK;
    /* Card, minigame and sack are dismissed with B or a swipe and take no
     * directional focus. They still hand back a set -- never nullptr -- because
     * the input layer walks whatever it is given. */
    case 1: return &FOCUS_CARD;
    case 6: case 7: return &FOCUS_NONE;
    default:
      /* The main screen hosts the confirm dialog as an overlay, so it owns the
       * focus while it is up. */
      if (millis() < confirmUntil) return &FOCUS_CONFIRM;
      /* Before the feed menu: the two never coexist in practice, and this one is
       * the irreversible decision, so it wins any tie. */
      if (millis() < choiceUntil) return &FOCUS_CHOICE;
      if (millis() < feedMenuUntil) return &FOCUS_FEED;
      /* An egg is not a menu. Upstream cracks it by tapping it, and the button
       * equivalent should be "press A", not "walk the cursor onto the right
       * rectangle first" -- so it offers exactly one target, the pet zone, and
       * every A press lands on it whatever the focus index says.
       *
       * Inside this case on purpose. Checked before the switch, it applied to
       * every screen, so opening the settings screen while the pet was still an
       * egg handed back the pet zone instead of the settings widgets -- which is
       * why the language could not be changed there. Same divergence as before:
       * a condition that is about one screen has to be asked about that screen. */
      if (pet.isEgg()) return &FOCUS_EGG;
      return &FOCUS_MAIN;
  }
}

bool tamapoke_is_dimmed(void) { return dimStage > 0; }

void tamapoke_wake(void) {
  dimStage = 0;
  note_activity(millis());
}

void tamapoke_hold_release(void) {
  uint32_t now = millis();
  if (!pet.isEgg() && pet.ceremony == CER_NONE && now >= confirmUntil) {
    confirmUntil = now + CONFIRM_MS;
    tamapoke_input_reset(0);
  }
}

/* ---- screen stubs (Scope 4) ---- */

static void renderStarterSelect() {
  gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);
  centerText(T(S_CHOOSE_STARTER), TP_CX, 4, 1);
  /* The focused row has to be obvious. This screen is the first thing a new save
   * shows and it drew three identical outlines with nothing marking the
   * selection: on a device with no touch panel that is not a cosmetic gap, it
   * means the player cannot tell what A will pick -- reported from hardware as
   * "the starter screen does nothing". Same treatment the confirm buttons get:
   * the selected row inverts and takes a thicker ring, so a still frame reads
   * correctly too. */
  uint8_t f = (tamapoke_current_focus_set() == &FOCUS_STARTER)
              ? tamapoke_input_focus() : 0xFF;
  for (uint8_t i = 0; i < 3; i++) {
    int16_t ry = STARTER_ROW_Y0 + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    int8_t fi = flashIdxForDex(CLASSIC_DEX[i]);
    bool sel = (f == i);

    uint16_t lab = drawTile(STARTER_ROW_X, ry, STARTER_ROW_W, STARTER_ROW_H,
                            TP_R_MD, sel, UI_WHITE);
    /* The sprite keeps its own colours on the selected row: the alternative flag
     * drawMap() takes is a black silhouette, which is what disappears against a
     * saturated fill. Colour reads on either background. */
    if (fi >= 0)
      drawMap(SPECIES[fi].sprite, SPRITE_H, STARTER_ROW_X + 4, ry + 4, 1, false);
    gfx->setTextSize(1);
    gfx->setTextColor(lab);
    gfx->setCursor(STARTER_ROW_X + 50, ry + STARTER_ROW_H / 2 - 4);
    gfx->print(DEX_TBL[CLASSIC_DEX[i]].name);
  }
  gfx->setTextColor(inkColor());
  /* The band below the last row was empty, and this screen is the first thing a
   * new save shows: nothing on it said the D-pad walks the rows or that A picks
   * one, which is how "the starter screen does nothing" gets reported twice. */
  drawKeyHint(TP_CX, STARTER_HINT_Y, true, 'A');
}

static void renderCard() {
  gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);
  uint16_t ink = inkColor();
  const char *name = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  char buf[32];
  snprintf(buf, sizeof(buf), "Nv.%u", pet.level());
  int8_t fi = flashIdxForDex(pet.speciesId);
  if (fi >= 0)
    drawMap(SPECIES[fi].sprite, SPRITE_H, TP_CX - 32, 16, 2, false);

  /* The three stat lines read as one block, so they get one well rather than
   * floating on the background at three different baselines. */
  drawWell(CARD_STATS_X, CARD_STATS_Y, CARD_STATS_W, CARD_STATS_H, TP_R_SM);
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  gfx->setCursor(CARD_STATS_X + 6, CARD_STATS_Y + 6);
  gfx->printf("ATK %u  DEF %u  SPE %u", pet.atkStat(), pet.defStat(), pet.speStat());
  gfx->setCursor(CARD_STATS_X + 6, CARD_STATS_Y + 20);
  gfx->printf("WGT %u  BOND %u", pet.weight, pet.bond);
  gfx->setCursor(CARD_STATS_X + 6, CARD_STATS_Y + 34);
  gfx->printf(T(S_MEDALS_FMT), pet.totalMedals, MED_COUNT);

  /* The two things this screen can do, and which one is selected. Without them the
   * card was a dead end: upstream renames from the name and trains from a button,
   * and neither was drawn or reachable here. */
  uint8_t f = (tamapoke_current_focus_set() == &FOCUS_CARD)
              ? tamapoke_input_focus() : 0xFF;

  /* Three tiles in the one focus language, where this screen used to have three
   * different ones: two stacked hairlines for the name, a fill-plus-border for
   * TRAIN, and a bare outline for BACK. */
  uint16_t lab = drawTile(0, 0, GFX_WIDTH, CARD_NAME_H, TP_R_XS, f == 0,
                          gNight ? UI_BG_NIGHT : UI_BG_DAY);
  gfx->setTextSize(1);
  gfx->setTextColor(f == 0 ? lab : ink);
  gfx->setCursor(4, 2);
  gfx->print(name);
  gfx->setCursor(GFX_WIDTH - 40, 2);
  gfx->print(buf);
  gfx->setTextColor(ink);

  lab = drawTile(CARD_TRAIN_X, CARD_TRAIN_Y, CARD_TRAIN_W, CARD_TRAIN_H, TP_R_SM,
                 f == 1, UI_WHITE);
  tileLabel(T(S_TRAIN_STR), CARD_TRAIN_X, CARD_TRAIN_Y, CARD_TRAIN_W,
            CARD_TRAIN_H, 1, lab);

  lab = drawTile(TP_CX - CARD_BACK_W / 2, CARD_BACK_Y, CARD_BACK_W, CARD_BACK_H,
                 TP_R_SM, f == 2, UI_WHITE);
  tileLabel(T(S_BACK), TP_CX - CARD_BACK_W / 2, CARD_BACK_Y, CARD_BACK_W,
            CARD_BACK_H, 1, lab);
}

/* One gallery thumbnail, centred on (cx, cy).
 *
 * The entry carries its own size and palette (see SdThumb); this used to read a
 * fixed 24x24 block of raw bytes and look each one up with spriteColor(), the
 * ASCII-sprite palette. So it drew the w/h/palette header as if those bytes were
 * pixels, ran ~240 bytes past the end of every record into the next species, and
 * coloured what it found through the wrong table -- which is the "the Pokedex
 * images are corrupted" report, and it was corrupt for every single entry.
 *
 * Centred rather than top-left because the entries are not one size: 14x24 up to
 * 17x24 across the 151, so a fixed corner puts each species somewhere different
 * inside its cell. */
static void drawThumbAt(int16_t dex, int16_t cx, int16_t cy, uint8_t s, bool sil) {
  SdThumb t;
  if (thumbs.get(dex, &t)) {
    int16_t x = cx - (int16_t)(t.w * s) / 2;
    int16_t y = cy - (int16_t)(t.h * s) / 2;
    for (uint8_t r = 0; r < t.h; r++)
      for (uint8_t c = 0; c < t.w; c++) {
        uint8_t v = t.px[(uint32_t)r * t.w + c];
        /* 0xFF is the transparent index, and so is anything outside the palette:
         * a record that names a colour it did not ship is not a colour. */
        if (v >= t.palCount) continue;
        uint16_t col = sil ? INK_K
                           : (uint16_t)(t.pal[2 * v] | (t.pal[2 * v + 1] << 8));
        gfx->fillRect(x + c * s, y + r * s, s, s, col);
      }
    return;
  }
  int8_t fi = flashIdxForDex(dex);
  if (fi >= 0 && fi < NUM_SPECIES) {
    int16_t sz = SPRITE_W * s;
    drawMap(SPECIES[fi].sprite, SPRITE_H, cx - sz / 2, cy - sz / 2, s, sil);
  } else {
    centerText("?", cx, cy - GFX_GLYPH_H, 2);
  }
}

static void renderGallery() {
  gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);
  uint16_t ink = inkColor();

  /* Detail view: one big sprite + name + back hint */
  if (galleryDetail > 0) {
    int16_t dex = galleryDetail;
    const DexEntry &e = DEX_TBL[dex];
    bool reg = pet.isRegistered(dex);
    bool shiny = pet.isShinyRegistered(dex);

    gfx->fillCircle(TP_CX, GAL_DET_SPRITE_CY, 50, UI_TRACK);
    if (galleryPmd.loaded) {
      drawPmdActM(galleryPmd, PMD_IDLE, TP_CX, GAL_DET_SPRITE_CY + 30,
                  reg ? millis() : 0, reg, !reg, 4);
    } else {
      /* The detail view is a whole screen for one species; at scale 1 the
       * thumbnail was a 16px stamp in the middle of a 100px disc. */
      drawThumbAt(dex, TP_CX, GAL_DET_SPRITE_CY, 3, !reg);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%s %s", dex,
             shiny ? "*" : "", e.name);
    centerText(buf, TP_CX, GAL_DET_HEAD_Y, 2);
    if (!reg) {
      centerText("???", TP_CX, GAL_DET_HEAD_Y + 16, 1);
    }
    centerText(T(S_BACK), TP_CX, GAL_DET_BACK_Y, 1);
    return;
  }

  /* Grid mode */
  char buf[24];
  snprintf(buf, sizeof(buf), "%u/151  %u/%u",
           pet.registeredCount(), galleryPage + 1,
           (DEX_COUNT + 15) / 16);
  centerText(buf, TP_CX, 6, 1);

  uint8_t focus = tamapoke_input_focus();
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t r = i / GAL_COLS, c = i % GAL_COLS;
    int16_t x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
    int16_t dex = galleryPage * 16 + i + 1;
    if (dex > DEX_COUNT) continue;
    bool reg = pet.isRegistered(dex);
    bool shiny = pet.isShinyRegistered(dex);
    /* The cell is inset by the ring thickness so a focused cell's plate cannot
     * paint over its neighbour -- a 44px pitch with a 42px cell has 2px to give
     * and the ring wants 3. */
    drawTile(x + 1, y + 1, GAL_CELL - GAL_GAP - 2, GAL_CELL - GAL_GAP - 2,
             TP_R_SM, focus == i, reg ? UI_WHITE : UI_TRACK);
    int16_t ccx = x + (GAL_CELL - GAL_GAP) / 2, ccy = y + (GAL_CELL - GAL_GAP) / 2;
    if (reg) {
      drawThumbAt(dex, ccx, ccy, 1, false);
      if (shiny) {
        gfx->setTextSize(1);
        gfx->setTextColor(TP_ACCENT);
        gfx->setCursor(x + GAL_CELL - 14, y + 2);
        gfx->print("*");
        gfx->setTextColor(ink);
      }
    } else {
      char nb[4];
      snprintf(nb, sizeof(nb), "%u", dex);
      centerText(nb, ccx, ccy - GFX_GLYPH_H / 2, 1);
    }
  }

  /* Page dots */
  uint8_t pages = (DEX_COUNT + 15) / 16;
  int16_t dots_w = pages * 4;
  int16_t dx = TP_CX - dots_w / 2;
  for (uint8_t p = 0; p < pages; p++) {
    int16_t px = dx + p * 4;
    if (p == galleryPage) gfx->fillCircle(px, 214, 2, ink);
    else                  gfx->drawCircle(px, 214, 2, UI_TRACK);
  }
}

static void renderKeyboard() {
  gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);
  uint16_t ink = inkColor();

  /* Title */
  centerText(T(S_NAME), TP_CX, KB_TITLE_Y, 2);

  /* Name preview: a well, not a tile -- it is a readout, not a target. */
  drawWell(KB_NAME_X, KB_NAME_Y, KB_NAME_W, KB_NAME_H, KB_NAME_R);
  gfx->setTextSize(2);
  gfx->setTextColor(ink);
  int16_t text_w = textWidth(nameBuf, 2);
  gfx->setCursor(TP_CX - text_w / 2,
                 KB_NAME_Y + (KB_NAME_H - 2 * GFX_GLYPH_H) / 2);
  gfx->print(nameBuf);

  /* 30 keys: 0..27 letters + . -, 28 = DEL, 29 = OK */
  uint8_t focus = tamapoke_input_focus();
  for (uint8_t i = 0; i < 30; i++) {
    uint8_t r = i / KB_COLS, c = i % KB_COLS;
    int16_t x = KB_X + c * KB_W, y = KB_Y + r * KB_H;
    bool focused = (focus == i);
    bool special = (i >= KB_SPECIAL0);
    /* OK is the primary action and keeps its green; DEL is a utility key and takes
     * the quiet surface. Everything else is a plain tile. */
    uint16_t fill = (i == KB_SPECIAL1) ? UI_BAR_OK
                                       : (special ? UI_TRACK : UI_WHITE);
    uint16_t lab = drawTile(x + 2, y + 2, KB_W - 4, KB_H - 4, TP_R_SM, focused,
                            fill);
    const char *label;
    char buf[2];
    if (i == KB_SPECIAL0) label = "<";
    else if (i == KB_SPECIAL1) label = "OK";
    else { buf[0] = KB_KEYS[i]; buf[1] = 0; label = buf; }
    tileLabel(label, x + 2, y + 2, KB_W - 4, KB_H - 4, KB_TEXT_SIZE, lab);
  }
}

static void drawClockBtn(int16_t x, int16_t y, const char *label, bool focused) {
  uint16_t lab = drawTile(x, y, CLOCK_BTN_W, CLOCK_BTN_H, TP_R_SM, focused,
                          UI_WHITE);
  tileLabel(label, x, y, CLOCK_BTN_W, CLOCK_BTN_H, 2, lab);
}

/* A settings pill: label on the left of its own row, value inside the tile. */
static void drawClockPill(int16_t x, int16_t w, const char *value, bool focused) {
  uint16_t lab = drawTile(x, CLOCK_PILL_Y, w, CLOCK_PILL_H, TP_R_MD, focused,
                          UI_WHITE);
  tileLabel(value, x, CLOCK_PILL_Y, w, CLOCK_PILL_H, 1, lab);
}

static void renderClock() {
  gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);
  uint16_t ink = inkColor();

  centerText(T(S_SET_TIME), TP_CX, CLOCK_TITLE_Y, CLOCK_TITLE_SIZE);

  /* HH:MM readout */
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", clockH, clockM);
  centerText(buf, TP_CX, CLOCK_TIME_Y, CLOCK_TIME_SIZE);

  /* 4 H/M buttons */
  uint8_t focus = tamapoke_input_focus();
  drawClockBtn(CLOCK_HMINUS_X, CLOCK_BTN_Y, "H-", focus == 0);
  drawClockBtn(CLOCK_HPLUS_X,  CLOCK_BTN_Y, "H+", focus == 1);
  drawClockBtn(CLOCK_MMINUS_X, CLOCK_BTN_Y, "M-", focus == 2);
  drawClockBtn(CLOCK_MPLUS_X,  CLOCK_BTN_Y, "M+", focus == 3);

  /* HOUR/MIN labels under the pairs */
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  gfx->setCursor(CLOCK_HPLUS_X + CLOCK_BTN_W / 2 - 2 * GFX_GLYPH_W,
                 CLOCK_PAIR_LBL_Y);
  gfx->print(T(S_HOUR));
  gfx->setCursor(CLOCK_MPLUS_X + CLOCK_BTN_W / 2 - 2 * GFX_GLYPH_W,
                 CLOCK_PAIR_LBL_Y);
  gfx->print(T(S_MIN));

  /* Sound and language: two pills on one row, same tile as everything else. The
   * name table is indexed by gLang -- langName() owns the static_assert that adding
   * a language without extending it is a compile error, which is what adding
   * Korean would otherwise have been: a read past the end into print(). */
  drawClockPill(CLOCK_SOUND_X, CLOCK_SOUND_W,
                audioEnabled() ? T(S_SND_ON) : T(S_SND_OFF), focus == 4);
  drawClockPill(CLOCK_LANG_X, CLOCK_LANG_W, langName(gLang), focus == 5);

  /* OK keeps its green whether focused or not -- it is the primary action, and a
   * tile that only becomes green when you point at it is not telling you that. */
  uint16_t lab = drawTile(CLOCK_OK_X, CLOCK_OK_Y, CLOCK_OK_W, CLOCK_OK_H,
                          TP_R_MD, focus == 6, UI_BAR_OK);
  tileLabel(T(S_YES), CLOCK_OK_X, CLOCK_OK_Y, CLOCK_OK_W, CLOCK_OK_H,
            CLOCK_OK_SIZE, lab);

  /* What the two labelled keys do, in the same pictogram style as everywhere
   * else: the D-pad walks the row, A presses. */
  drawKeyHint(TP_CX, CLOCK_HINT_Y, false, 'A');
}

static void renderGame() {
  /* This screen has its own dark background, day or night, so it cannot borrow
   * inkColor() -- which is dark for the light theme. The score was being drawn in
   * it: dark text on dark navy, legible only if you knew it was there. */
  gfx->fillScreen(GAME_BG);
  uint32_t now = millis();

  char buf[24];
  snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
  gfx->setTextColor(UI_WHITE);
  centerText(buf, TP_CX, GAME_SCORE_Y, GAME_SCORE_SIZE);
  /* The record, so a score means something, and one dot per life left. Both
   * constants existed in the header and nothing drew them. */
  snprintf(buf, sizeof(buf), T(S_REC_FMT), pet.gameHi);
  gfx->setTextColor(UI_TRACK);
  centerText(buf, TP_CX, GAME_RECORD_Y, 1);
  for (uint8_t i = 0; i < GAME_LIVES; i++) {
    int16_t lx = GAME_LIVES_X0 + i * GAME_LIVES_DX;
    if (i < GAME_LIVES - gameMisses)
      gfx->fillCircle(lx, GAME_LIVES_Y + GAME_LIVES_R, GAME_LIVES_R, UI_BAR_BAD);
    else
      gfx->drawCircle(lx, GAME_LIVES_Y + GAME_LIVES_R, GAME_LIVES_R, UI_TRACK);
  }

  gfx->fillRect(0, GAME_TOP, GFX_WIDTH, 1, UI_TRACK);
  gfx->fillCircle((int16_t)ballX, (int16_t)ballY, GAME_BALL_R, UI_BAR_BAD);
  gfx->fillRoundRect((int16_t)paddleX, GAME_PADDLE_Y, GAME_PADDLE_W,
                     GAME_PADDLE_H, TP_R_XS, UI_WHITE);

  /* Which keys move it. The paddle was the only thing here that answered a key
   * and nothing on screen said so -- and for three releases nothing did. */
  if (!gameOverUntil && now - gameStartedAt < GAME_HINT_MS) {
    drawKeyHintOn(TP_CX, GAME_HINT_Y, false, 0, UI_TRACK, GAME_BG);
  }

  if (gameOverUntil) {
    gfx->setTextColor(UI_WHITE);
    centerText(T(gameNewHi ? S_NEW_RECORD : S_GREAT_JOY), TP_CX,
               GAME_OVER_LABEL_Y, 2);
    snprintf(buf, sizeof(buf), T(S_PLUS_JOY));
    gfx->setTextColor(UI_BAR_OK);
    centerText(buf, TP_CX, GAME_OVER_LABEL_Y + 28, 1);
  }
  gfx->setTextColor(UI_WHITE);
}

static void renderSack() {
  uint32_t now = millis();
  uint16_t ink = inkColor();
  gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);

  /* Result screen: timed reveal of str gain / record. */
  if (now >= sackUntil && now < sackOverUntil) {
    char buf[24];
    gfx->setTextColor(ink);
    snprintf(buf, sizeof(buf), T(S_HITS_FMT), sackHits);
    centerText(buf, TP_CX, SACK_RESULT_Y, SACK_RESULT_SIZE);
    snprintf(buf, sizeof(buf), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_BAD);
    centerText(buf, TP_CX, SACK_RESULT_Y + 28, 2);
    if (sackNewHi) {
      gfx->setTextColor(TP_ACCENT);
      centerText(T(S_NEW_RECORD), TP_CX, SACK_RESULT_Y + 52, 1);
    }
    gfx->setTextColor(ink);
    return;
  }

  /* The sandbag: rope from the top of the screen, chain link into the
   * tied-off neck (taper), and the body itself with a horizontal seam.
   * Each hit applies sackShake which we feed into a horizontal sin offset
   * so the bag visibly recoils. */
  int8_t shake = (sackShake > 0) ? (int8_t)(sinf(now * 0.05f) * sackShake) : 0;
  if (sackShake > 0 && now - hitTime > 200) sackShake = 0;
  int16_t sx = SACK_BODY_X + shake;
  uint16_t rope   = C565(0x6a, 0x55, 0x36);
  uint16_t chain  = C565(0x9a, 0x9a, 0x9a);
  uint16_t taper  = C565(0x6a, 0x2a, 0x1a);
  uint16_t body   = C565(0xa4, 0x4a, 0x2a);
  uint16_t seam   = C565(0x5a, 0x22, 0x12);

  /* rope */
  gfx->fillRect(TP_CX - 2, SACK_ROPE_TOP_Y, 4,
                SACK_BODY_TOP_Y - SACK_ROPE_TOP_Y, rope);
  /* chain link just above the taper */
  gfx->fillRect(sx + SACK_BODY_W/2 - 4, SACK_BODY_TOP_Y - 6, 8, 8, chain);
  /* tied neck */
  gfx->fillRoundRect(sx, SACK_BODY_TOP_Y, SACK_BODY_W, SACK_TAPER_H, 6, taper);
  /* body */
  gfx->fillRoundRect(sx, SACK_BODY_TOP_Y + SACK_TAPER_H/2,
                     SACK_BODY_W, SACK_BODY_H, 18, body);
  /* horizontal seam across the middle */
  gfx->fillRect(sx + 4, SACK_SEAM_Y, SACK_BODY_W - 8, 3, seam);

  /* HUD. The ink is set explicitly: this screen never did, so the counter and the
   * hint came out in whatever colour the previous screen had left in the text
   * state -- white on cream, i.e. invisible, which is how a 24-point round read as
   * an empty screen. */
  char buf[24];
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  snprintf(buf, sizeof(buf), T(S_HITS_FMT), sackHits);
  centerText(buf, TP_CX, SACK_HIT_COUNTER_Y, SACK_HIT_COUNTER_SZ);
  centerText(T(S_HIT_FAST), TP_CX, SACK_HINT_Y, SACK_HINT_SIZE);
  /* WHICH key to hit fast. "HIT FAST!" was the entire instruction, on a screen
   * where no key did anything at all. */
  drawKeyHintOn(TP_CX, SACK_KEY_HINT_Y, false, 'A', ink,
                gNight ? UI_BG_NIGHT : UI_BG_DAY);

  /* time bar: full width at start, shrinks to zero as the timer runs out. */
  uint32_t remain = (sackUntil > now) ? (sackUntil - now) : 0;
  uint16_t fillW = (uint16_t)((uint64_t)remain * SACK_BAR_W / SACK_ROUND_MS);
  drawWell(SACK_BAR_X, SACK_BAR_Y, SACK_BAR_W, SACK_BAR_H, TP_R_SM);
  if (fillW > 0)
    gfx->fillRoundRect(SACK_BAR_X, SACK_BAR_Y, fillW, SACK_BAR_H, TP_R_SM,
                       UI_BAR_OK);
  gfx->drawRoundRect(SACK_BAR_X, SACK_BAR_Y, SACK_BAR_W, SACK_BAR_H, TP_R_SM, ink);
}

/* ---- render dispatcher ---- */

static void updateNight() {
  int h = sceneHour();
  gNight = (h >= 20 || h < 7);
}

/* Compute which screen id the runtime state currently maps to. Mirrors the
 * upstream render() branch order. harness_screen, when >=0, overrides. */
static int current_screen_id() {
  if (harness_screen >= 0) return harness_screen;
  if (pet.awaitingStarter()) return 5;
  if (galleryOpen) return 2;
  if (gameOpen)    return 6;
  if (sackOpen)    return 7;
  if (kbOpen)      return 3;
  if (clockOpen)   return 4;
  if (cardOpen)    return 1;
  return 0;
}

/* Main screen path -- everything that isn't a full-screen sub-mode. Also
 * hosts the overlay dialogs (bath / feed / confirm / choice). */
static void render_main_screen(uint32_t now) {
  uint8_t biome = 0;
  if (!pet.isEgg() && pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT)
    biome = DEX_TBL[pet.speciesId].biome;

  drawScene(biome, now, gNight);

  if (pet.ceremony != CER_NONE) { drawCeremony(); return; }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), UI_WHITE, T(eggMsg()));
    drawEgg();
    /* Paint the bottom band the egg screen has no widgets for. Returning without
     * it left y=152..240 holding whatever the previous screen had drawn there --
     * on hardware the third starter row (SQUIRTLE, with its outline) sat under
     * the egg, and coming back from the pause menu left that band black. The
     * canvas persists between frames on purpose (lcd_clone, for incremental
     * drawing), so a path that skips a region is not "leaving it blank", it is
     * showing the last screen that did draw there. */
    drawPanel();
    return;
  }

  ensureMon();

  const char *name = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  uint16_t accent = DEX_TBL[pet.speciesId].accent;
  drawHeader(name, accent, T(statusMsg()));

  if (pet.evolving()) {
    drawEvolveFX(now);
  } else {
    drawPet();
  }

  drawBath(now);
  drawPoops();

  drawPanel();

  drawBars();
  drawButtons();
  drawCelebration();

  if (!pet.evolving() && pet.wantEvolveButton()) {
    choiceKind = 1;
    choiceUntil = now + CHOICE_MS;
  }
  if (!pet.evolving() && pet.wantFarewellButton()) {
    choiceKind = 2;
    choiceUntil = now + CHOICE_MS;
  }
  if (pet.canRunawayNow()) drawRunawayButton(now);

  if (pet.sleeping) {
    gfx->setTextSize(2);
    gfx->setTextColor(inkColor());
    gfx->setCursor(TP_CX + 20, TP_PET_CY - 20);
    gfx->print("Zz");
  }

  if (now < feedMenuUntil) drawFeedMenu(now);
  if (now < confirmUntil) drawConfirmDialog();
  if (now < choiceUntil) drawChoiceDialog(now);
}

void tamapoke_ui_render(void) {
  updateNight();
  uint32_t now = millis();

  /* A screen change wipes the canvas first. The canvas survives between frames
   * by design (lcd_clone), which is what makes the incremental drawing cheap --
   * and also what makes a region no screen paints show the previous screen's
   * pixels. Every full-screen renderer starts with fillScreen, so this is
   * belt-and-braces for the one that forgets, and for the main screen whose
   * egg/pet/ceremony branches each cover a different area.
   *
   * Also resets the focus index: an index that was valid on the screen being
   * left is either invisible on the new one or, worse, points at a different
   * action than the one it highlights. */
  /* The signature is the screen id AND everything that changes which regions get
   * painted: the timed overlays, and the main screen's three mutually exclusive
   * branches (egg / ceremony / pet), each of which covers a different area. When
   * it changes, wipe the canvas first.
   *
   * Screen id alone was not enough. An overlay that expires leaves the same
   * screen id behind, so nothing repainted where it had been -- the bath bubbles
   * and the feed menu stayed on screen after their timers ran out. */
  int screen = current_screen_id();
  uint32_t sig = (uint32_t)(screen + 1);
  sig = sig * 31 + (now < bathUntil        ? 1u : 0u);
  sig = sig * 31 + (now < feedMenuUntil    ? 1u : 0u);
  sig = sig * 31 + (now < confirmUntil     ? 1u : 0u);
  sig = sig * 31 + (now < choiceUntil      ? 1u : 0u);
  sig = sig * 31 + (pet.isEgg()            ? 1u : 0u);
  sig = sig * 31 + (pet.ceremony != CER_NONE ? 1u : 0u);

  static uint32_t last_sig = 0;
  if (sig != last_sig || g_force_full_repaint) {
    bool screen_changed = (sig != last_sig);
    last_sig = sig;
    g_force_full_repaint = false;
    gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);
    /* Focus follows the screen: an index that was valid on the one being left is
     * either invisible here or points at a different action than it highlights.
     * Not reset on a bare force-repaint, which is the same screen redrawn. */
    if (screen_changed) tamapoke_input_reset(0);
  }

  switch (screen) {
    case 5:  renderStarterSelect();  break;
    case 2:  renderGallery();        break;
    case 6:  renderGame();           break;
    case 7:  renderSack();           break;
    case 3:  renderKeyboard();       break;
    case 4:  renderClock();          break;
    case 1:  renderCard();           break;
    default: render_main_screen(now); break;
  }
  gfx->flush();
}

/* ---- host harness API ---- */

bool tamapoke_ui_had_activity(void) { return g_had_activity; }

/* Defined here rather than next to the other harness hooks because it needs
 * drawThumbAt(), which is further down the file with the gallery it serves. */
void tamapoke_ui_draw_thumb(int16_t dex, int16_t cx, int16_t cy, uint8_t s) {
  drawThumbAt(dex, cx, cy, s, false);
}

void tamapoke_ui_goto_screen(int id) {
  harness_screen = (int8_t)id;
  uint32_t now = millis();

  /* Clear all transient state so each capture is reproducible. */
  galleryDetail = 0;
  cardPage = 0;
  galleryOpen = cardOpen = kbOpen = clockOpen = gameOpen = sackOpen = false;
  if (galleryPmd.loaded) galleryPmd.unload();
  bathUntil = feedMenuUntil = confirmUntil = choiceUntil = 0;
  /* A ceremony is transient state too, and this reset forgot it -- so a capture
   * taken after one had started rendered the farewell instead of the screen asked
   * for, and every gesture stayed blocked (onSwipe/onSwipeV refuse to run during a
   * ceremony, correctly). That is what made the navigation checks below look like a
   * wiring failure when the wiring was fine. */
  pet.ceremony = CER_NONE;

  /* Seed inputs that the screen needs to render anything meaningful. */
  switch (id) {
    case 0: /* main -- give the pet a species if it's an egg, so the
              * main scene draws a sprite rather than the egg shell. */
      if (pet.isEgg() || pet.speciesId < 1) {
        pet.chooseStarter(CLASSIC_DEX[0]);
        pet.speciesId = CLASSIC_DEX[0];
      }
      ensureMon();
      break;
    case 1: /* card */
      cardOpen = true;
      break;
    case 2: /* gallery */
      galleryOpen = true;
      if (!thumbs.loaded) thumbs.load();
      break;
    case 3: /* keyboard -- prefill a sample name so the box isn't empty */
      kbOpen = true;
      strcpy(nameBuf, "TEST");
      nameLen = strlen(nameBuf);
      break;
    case 4: /* clock */
      clockOpen = true;
      break;
    case 5: /* starter -- the pet is awaiting choice. newEgg() re-rolls
              * starterPick from registeredCount(); on a fresh harness boot
              * the dex is empty so this sets the flag the renderer keys on. */
      pet.newEgg();
      break;
    case 6: /* game -- spawn the ball and centre the paddle */
      gameOpen = true;
      ballX = GFX_WIDTH / 2;
      ballY = GAME_TOP + 30;
      ballVX = 1.5f; ballVY = 1.0f;
      paddleX = (GFX_WIDTH - GAME_PADDLE_W) / 2;
      paddleDir = 0;
      gameScore = 3; gameMisses = 1;
      gameOverUntil = 0; gameNewHi = false;
      gameStartedAt = now;
      break;
    case 7: /* sack */
      sackOpen = true;
      sackUntil = now + SACK_ROUND_MS;
      sackOverUntil = 0;
      sackHits = 24; sackGain = 2; sackShake = 0; sackNewHi = false;
      break;
    case 8: /* bath overlay -- force main path + bath timer */
      harness_screen = 0;
      if (pet.isEgg() || pet.speciesId < 1) {
        pet.chooseStarter(CLASSIC_DEX[0]);
        pet.speciesId = CLASSIC_DEX[0];
      }
      ensureMon();
      bathUntil = now + 3000;
      bathPending = true;
      for (uint8_t i = 0; i < 14; i++) {
        bubbles[i].x = random(40, 280);
        bubbles[i].y = random(60, 140);
        bubbles[i].r = random(3, 8);
        bubbles[i].ph = random(0, 64);
      }
      break;
    case 9: /* feed menu overlay */
      harness_screen = 0;
      if (pet.isEgg() || pet.speciesId < 1) {
        pet.chooseStarter(CLASSIC_DEX[0]);
        pet.speciesId = CLASSIC_DEX[0];
      }
      ensureMon();
      feedMenuUntil = now + FEED_MENU_MS;
      break;
    case 10: /* release-confirm overlay */
      harness_screen = 0;
      if (pet.isEgg() || pet.speciesId < 1) {
        pet.chooseStarter(CLASSIC_DEX[0]);
        pet.speciesId = CLASSIC_DEX[0];
      }
      ensureMon();
      confirmUntil = now + CONFIRM_MS;
      break;
    case 11: /* evolve/farewell choice overlay.
              * Added when it turned out to have no focus set: it was drawn by the
              * main path, so no capture covered it and the focus-visibility gate
              * never looked at it -- and it had no focus set at all, which made the
              * one irreversible decision in the game unreachable with buttons. */
      harness_screen = 0;
      if (pet.isEgg() || pet.speciesId < 1) {
        pet.chooseStarter(CLASSIC_DEX[0]);
        pet.speciesId = CLASSIC_DEX[0];
      }
      ensureMon();
      choiceKind = 1;
      choiceUntil = now + CHOICE_MS;
      break;
    default:
      break;
  }
}

const char *tamapoke_ui_screen_name(int id) {
  switch (id) {
    case 0:  return "main";
    case 1:  return "card";
    case 2:  return "gallery";
    case 3:  return "keyboard";
    case 4:  return "clock";
    case 5:  return "starter";
    case 6:  return "game";
    case 7:  return "sack";
    case 8:  return "bath";
    case 9:  return "feed";
    case 10: return "confirm";
    case 11: return "choice";
    default: return "unknown";
  }
}

/* ---- public API ---- */

void tamapoke_ui_init(void) {
  gfx->begin();
  pet.begin();
  randomSeed(tamapoke_epoch());
  for (uint8_t i = 0; i < 14; i++) {
    bubbles[i].x = random(40, 280);
    bubbles[i].y = random(60, 200);
    bubbles[i].r = random(3, 8);
    bubbles[i].ph = random(0, 64);
  }
  lastInteract = millis();
  dimStage = 0;
  beh.x = beh.targetX = TP_CX;
  behNext();
}

void tamapoke_ui_tick(uint32_t now_ms) {
  static uint32_t last_tick = 0;
  if (now_ms - last_tick < 33) return;
  last_tick = now_ms;

  /* had_activity reflects "since the previous tick". Clear up front; any
   * input handler that runs during this frame sets it via note_activity(). */
  g_had_activity = false;

  if (tamapoke_clock_is_set()) pet.syncClock(tamapoke_epoch());
  pet.update(now_ms);

  if (pet.savePending()) pet.flushSave();

  stepBeh();

  /* Ball round. The paddle is stepped here rather than in the input poll so its
   * speed comes from the 33 ms tick and not from however fast the main loop
   * happens to be spinning. */
  if (gameOpen && !pet.evolving() && !gameOverUntil) {
    paddleX += paddleDir * GAME_PADDLE_STEP;
    if (paddleX < GAME_PADDLE_X_MIN) paddleX = GAME_PADDLE_X_MIN;
    if (paddleX > GAME_PADDLE_X_MAX) paddleX = GAME_PADDLE_X_MAX;

    ballX += ballVX;
    ballY += ballVY;
    if (ballX < 4 || ballX > GFX_WIDTH - 4) ballVX = -ballVX;
    if (ballY < GAME_TOP) ballVY = -ballVY;
    if (ballY >= GAME_PADDLE_Y) {
      if (ballX >= paddleX && ballX <= paddleX + GAME_PADDLE_W) {
        ballVY = -fabsf(ballVY);
        ballVX += (ballX - paddleX - GAME_PADDLE_W / 2) * 0.1f;
        /* Clamped both ways: enough sideways motion that the ball cannot fall in
         * a vertical column the paddle never has to move for, and not so much
         * that it crosses the screen faster than the paddle can. random(-3,4)
         * used to be able to return 0 for exactly the first case. */
        if (ballVX > GAME_BALL_VX_MAX) ballVX = GAME_BALL_VX_MAX;
        if (ballVX < -GAME_BALL_VX_MAX) ballVX = -GAME_BALL_VX_MAX;
        if (ballVX >= 0 && ballVX < GAME_BALL_VX_MIN) ballVX = GAME_BALL_VX_MIN;
        if (ballVX < 0 && ballVX > -GAME_BALL_VX_MIN) ballVX = -GAME_BALL_VX_MIN;
        if (gameScore < 255) gameScore++;
        sfxPlay(SFX_TAP);
      } else {
        gameMisses++;
        if (gameMisses >= GAME_LIVES) {
          endGame();
        } else {
          ballY = GAME_TOP + 20;
          ballVX = (random(0, 2) ? 1 : -1) * (1.0f + random(0, 20) / 10.0f);
          ballVY = 2.0f + random(0, 20) / 10.0f;
        }
      }
    }
  }
  /* The result screen, and the only way either game ends: they used to have no
   * exit at all except pressing B. */
  if (gameOpen && gameOverUntil && now_ms >= gameOverUntil) {
    gameOpen = false;
    gameOverUntil = 0;
  }

  /* Training round: the timer expiring is what banks the hits. */
  if (sackOpen) {
    if (sackShake > 0 && now_ms - hitTime > SACK_SHAKE_MS) sackShake = 0;
    if (now_ms >= sackUntil && !sackOverUntil) endSack();
    if (sackOverUntil && now_ms >= sackOverUntil) {
      sackOpen = false;
      sackOverUntil = 0;
    }
  }

  if (now_ms - lastInteract > 30000) dimStage = 1;
  if (now_ms - lastInteract > 60000) dimStage = 2;

  tamapoke_ui_render();
}
