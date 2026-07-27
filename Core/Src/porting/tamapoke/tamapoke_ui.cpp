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
#define PANEL_DAY   C565(0x38, 0x38, 0x44)
#define PANEL_NIGHT C565(0x10, 0x10, 0x18)

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
static float ballX, ballY, ballVX, ballVY, gamePetX, paddleX;
static float hitX, hitY, sackShake;
static uint32_t lastInteract, holdStart;
/* True if any input handler ran since the previous tick. Cleared at the top
 * of tamapoke_ui_tick; exposed via tamapoke_ui_had_activity(). */
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
static const focus_set_t FOCUS_CONFIRM = {CONFIRM_BOXES, ARRAY_LEN(CONFIRM_BOXES), 0, 0};

static const focus_set_t FOCUS_GALLERY = {nullptr, 16, GAL_COLS, 1};
static const focus_set_t FOCUS_KEYBOARD = {nullptr, 30, KB_COLS, 2};

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
static const focus_set_t FOCUS_STARTER = {STARTER_BOXES, ARRAY_LEN(STARTER_BOXES), 0, 0};

/* A screen that takes no directional focus still has to answer the question,
 * because the input layer walks whatever it is handed. Answering nullptr made
 * every button press on the starter/card/clock/game/sack screens dereference it:
 * focus_step() read fs->count through address 0, and the value it found in ITCM
 * was odd, so the following halfword load raised a UsageFault with
 * CFSR=0x01000000 (UNALIGNED) -- reported on device as a crash on any keypress.
 * An empty set is the honest answer: nothing to walk, nothing to tap, and B/back
 * and the swipes keep working because they never consult the set. */
static const focus_set_t FOCUS_NONE = {nullptr, 0, 0, 0};

/* ---- helpers ---- */

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

static void drawMap(const char *const *map, uint8_t n,
                    int16_t x, int16_t y, int16_t s, bool sil) {
  for (uint8_t r = 0; r < n; r++) {
    const char *row = map[r];
    if (!row) break;
    for (uint8_t c = 0; c < n && row[c]; c++) {
      char ch = row[c];
      if (ch == '.') continue;
      uint16_t col = sil ? INK_K : spriteColor(ch);
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
      if (idx == 0) continue;
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

  gfx->fillRect(0, TP_HORIZON, GFX_WIDTH, TP_PET_GROUND - TP_HORIZON, soil);
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
  uint16_t ink = inkColor();
  uint32_t now = millis();
  uint8_t f = tamapoke_input_focus();
  bool focused = (tamapoke_current_focus_set() == &FOCUS_MAIN && f >= 1 && f <= 4);
  /* touch is gone -- the cursor IS the input. Make it impossible to miss:
   * focused button inverts fill, gets a 3px warning-coloured ring drawn
   * around it, and pulses so a still frame also reads as "selected". */
  uint8_t pulse = focused ? (uint8_t)(sinf(now * 0.012f) * 1.5f + 1.5f) : 0;
  for (uint8_t i = 0; i < 4; i++) {
    int16_t bx = BTN_XS[i];
    bool sel = focused && (f - 1 == i);
    uint16_t fill = sel ? ink : (pet.sleeping ? UI_TRACK : UI_WHITE);
    gfx->fillRoundRect(bx, BTN_ROW_Y, BTN_W, BTN_H, BTN_R, fill);
    gfx->drawRoundRect(bx, BTN_ROW_Y, BTN_W, BTN_H, BTN_R, ink);
    drawMap(ICONS[i], 16, bx + BTN_HALF - 8, BTN_ROW_Y + BTN_HALF - 8, 1, false);
    if (sel) {
      /* 3px-thick ring just outside the button, expanded by the pulse. */
      for (uint8_t r = 0; r < 3; r++) {
        int16_t off = r + pulse;
        gfx->drawRoundRect(bx - off, BTN_ROW_Y - off,
                           BTN_W + 2*off, BTN_H + 2*off,
                           BTN_R + off, UI_BAR_WARN);
      }
    }
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
  gfx->fillRoundRect(bx, by, bw, 20, 6, UI_BAR_WARN);
  gfx->drawRoundRect(bx, by, bw, 20, 6, UI_WHITE);
  centerText(txt, TP_CX, by + 2, 2);
}

/* ---- CTA buttons ---- */

static void drawEvolveButton(uint32_t now) {
  uint8_t p = (uint8_t)(sinf(now * 0.006f) * 2 + 2);
  gfx->fillRoundRect(EVO_BTN_X - p, EVO_BTN_Y - p,
                     EVO_BTN_W + 2*p, EVO_BTN_H + 2*p, 8, UI_BAR_BAD);
  gfx->drawRoundRect(EVO_BTN_X, EVO_BTN_Y, EVO_BTN_W, EVO_BTN_H, 8, UI_WHITE);
  centerText(T(S_EVO_TAP), TP_CX, EVO_BTN_Y + EVO_BTN_H/2 - GFX_GLYPH_H, 2);
}

static void drawFarewellButton(uint32_t now) {
  uint8_t p = (uint8_t)(sinf(now * 0.005f) * 2 + 2);
  gfx->fillRoundRect(FAR_BTN_X - p, FAR_BTN_Y - p,
                     FAR_BTN_W + 2*p, FAR_BTN_H + 2*p, 8, UI_BAR_WARN);
  gfx->drawRoundRect(FAR_BTN_X, FAR_BTN_Y, FAR_BTN_W, FAR_BTN_H, 8, UI_WHITE);
  char buf[32];
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), pet.nick[0] ? pet.nick : "?");
  centerText(buf, TP_CX, FAR_BTN_Y + FAR_BTN_H/2 - GFX_GLYPH_H, 1);
}

static void drawRunawayButton(uint32_t now) {
  uint16_t dark = C565(0x3a, 0x44, 0x5a);
  gfx->fillRoundRect(RUN_BTN_X, RUN_BTN_Y, RUN_BTN_W, RUN_BTN_H, 8, dark);
  gfx->drawRoundRect(RUN_BTN_X, RUN_BTN_Y, RUN_BTN_W, RUN_BTN_H, 8, UI_WHITE);
  centerText(T(S_RUNAWAY_BTN), TP_CX, RUN_BTN_Y + RUN_BTN_H/2 - GFX_GLYPH_H, 1);
}

/* ---- choice dialog ---- */

static void drawChoiceDialog(uint32_t now) {
  uint16_t ink = inkColor();
  gfx->fillRoundRect(CHOICE_DIALOG_X, CHOICE_DIALOG_Y,
                     CHOICE_DIALOG_W, CHOICE_DIALOG_H, 10,
                     gNight ? UI_BG_NIGHT : UI_BG_DAY);
  gfx->drawRoundRect(CHOICE_DIALOG_X, CHOICE_DIALOG_Y,
                     CHOICE_DIALOG_W, CHOICE_DIALOG_H, 10, ink);
  const char *q = (choiceKind == 1) ? T(S_EVO_Q) : T(S_FAR_Q);
  centerText(q, TP_CX, CHOICE_DIALOG_Y + 8, 1);
  uint16_t actCol = (choiceKind == 1) ? UI_BAR_BAD : UI_BAR_WARN;
  gfx->fillRoundRect(EVO_BTN_X, EVO_BTN_Y, EVO_BTN_W, EVO_BTN_H, 6, actCol);
  centerText((choiceKind == 1) ? T(S_EVO_TAP) : T(S_FAR_GO),
             TP_CX, EVO_BTN_Y + EVO_BTN_H/2 - GFX_GLYPH_H, 1);
  uint16_t keepCol = (choiceKind == 1) ? UI_TRACK : UI_BAR_OK;
  gfx->fillRoundRect(FAR_BTN_X, FAR_BTN_Y, FAR_BTN_W, FAR_BTN_H, 6, keepCol);
  centerText((choiceKind == 1) ? T(S_EVO_KEEP) : T(S_FAR_STAY),
             TP_CX, FAR_BTN_Y + FAR_BTN_H/2 - GFX_GLYPH_H, 1);
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
  uint16_t ink = inkColor();
  gfx->fillRoundRect(FEED_MENU_X, FEED_MENU_Y, FEED_MENU_W, FEED_MENU_H,
                     FEED_MENU_R, gNight ? UI_BG_NIGHT : UI_BG_DAY);
  gfx->drawRoundRect(FEED_MENU_X, FEED_MENU_Y, FEED_MENU_W, FEED_MENU_H,
                     FEED_MENU_R, ink);
  static const char *const *ICONS[5] = {
    SPR_ICON_FOOD, nullptr, nullptr, nullptr, nullptr
  };
  for (uint8_t i = 0; i < 5; i++) {
    int16_t ix = FEED_ICON0_X + i * FEED_ICON_GAP;
    gfx->fillRect(ix, FEED_ICON_Y, FEED_ICON_SZ, FEED_ICON_SZ,
                  i % 2 ? UI_BAR_WARN : UI_BAR_OK);
    gfx->drawRoundRect(ix, FEED_ICON_Y, FEED_ICON_SZ, FEED_ICON_SZ, 2, ink);
  }
}

/* ---- release confirm ---- */

static void drawConfirmDialog() {
  uint16_t ink = inkColor();
  gfx->fillRoundRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, CONFIRM_R,
                     gNight ? UI_BG_NIGHT : UI_BG_DAY);
  gfx->drawRoundRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, CONFIRM_R, ink);
  char buf[48];
  snprintf(buf, sizeof(buf), T(S_RELEASE_FMT),
           pet.nick[0] ? pet.nick : dexName(pet.speciesId));
  centerText(buf, TP_CX, CONFIRM_Y + 12, 1);
  uint8_t f = (tamapoke_current_focus_set() == &FOCUS_CONFIRM)
              ? tamapoke_input_focus() : 0xFF;
  uint16_t yesFill = (f == 0) ? UI_WHITE   : UI_BAR_BAD;
  uint16_t yesTxt  = (f == 0) ? UI_BAR_BAD : UI_WHITE;
  uint16_t noFill  = (f == 1) ? UI_WHITE   : UI_BAR_OK;
  uint16_t noTxt   = (f == 1) ? UI_BAR_OK  : UI_WHITE;
  gfx->fillRoundRect(CONFIRM_YES_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H,
                     4, yesFill);
  gfx->drawRoundRect(CONFIRM_YES_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H,
                     4, ink);
  centerText(T(S_YES), CONFIRM_YES_X + CONFIRM_BTN_W/2,
             CONFIRM_BTN_Y + CONFIRM_BTN_H/2 - GFX_GLYPH_H, 1);
  gfx->setTextColor(yesTxt);
  centerText(T(S_YES), CONFIRM_YES_X + CONFIRM_BTN_W/2,
             CONFIRM_BTN_Y + CONFIRM_BTN_H/2 - GFX_GLYPH_H, 1);
  gfx->fillRoundRect(CONFIRM_NO_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H,
                     4, noFill);
  gfx->drawRoundRect(CONFIRM_NO_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H,
                     4, ink);
  gfx->setTextColor(noTxt);
  centerText(T(S_NO), CONFIRM_NO_X + CONFIRM_BTN_W/2,
             CONFIRM_BTN_Y + CONFIRM_BTN_H/2 - GFX_GLYPH_H, 1);
  gfx->setTextColor(ink);
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
  if (cardOpen) { cardOpen = false; return; }
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
        case 0: feedMenuUntil = now + 3000; break;
        case 1: pet.play(); break;
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
  if (galleryOpen && galleryPage > 0) galleryPage--;
  note_activity(millis());
  dimStage = 0;
}

void onSwipeV(int dir) {
  if (!galleryOpen) return;
  if (galleryPage < (DEX_COUNT - 1) / 16) galleryPage++;
  note_activity(millis());
  dimStage = 0;
}

void onBack(void) {
  uint32_t now = millis();
  note_activity(now);
  dimStage = 0;
  if (kbOpen) { kbOpen = false; pet.rename(nameBuf); return; }
  if (cardOpen) { cardOpen = false; return; }
  if (galleryOpen) { galleryOpen = false; return; }
  if (clockOpen) { clockOpen = false; return; }
  if (gameOpen) { gameOpen = false; return; }
  if (sackOpen) { sackOpen = false; return; }
}

/* ---- focus / dim / hold ---- */

const focus_set_t *tamapoke_current_focus_set(void) {
  if (kbOpen) return &FOCUS_KEYBOARD;
  if (galleryOpen) return &FOCUS_GALLERY;
  if (millis() < confirmUntil) return &FOCUS_CONFIRM;
  /* An explicitly opened screen outranks the pet's awaiting-starter flag. The
   * other order let a screen that is plainly on top be driven by the starter
   * rows underneath it: the minigame and the sack answered FOCUS_STARTER while
   * the pet still had no species, so the D-pad walked rows nobody could see.
   * Never nullptr either -- see FOCUS_NONE. These screens are dismissed with B
   * or a swipe, so they take no directional focus, but they must still hand back
   * a set the caller can read. */
  if (kbOpen || clockOpen || cardOpen || gameOpen || sackOpen) return &FOCUS_NONE;
  if (pet.awaitingStarter()) return &FOCUS_STARTER;
  return &FOCUS_MAIN;
}

bool tamapoke_is_dimmed(void) { return dimStage > 0; }

void tamapoke_wake(void) {
  dimStage = 0;
  note_activity(millis());
}

void tamapoke_hold_release(void) {
  uint32_t now = millis();
  if (!pet.isEgg() && pet.ceremony == CER_NONE && now >= confirmUntil) {
    confirmUntil = now + 10000;
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
  uint16_t ink = inkColor();
  for (uint8_t i = 0; i < 3; i++) {
    int16_t ry = STARTER_ROW_Y0 + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    int8_t fi = flashIdxForDex(CLASSIC_DEX[i]);
    bool sel = (f == i);

    if (sel) {
      gfx->fillRoundRect(STARTER_ROW_X, ry, STARTER_ROW_W, STARTER_ROW_H, 6, ink);
      for (uint8_t t = 0; t < 3; t++)
        gfx->drawRoundRect(STARTER_ROW_X - t, ry - t, STARTER_ROW_W + 2 * t,
                           STARTER_ROW_H + 2 * t, 6, UI_BAR_WARN);
    } else {
      gfx->drawRoundRect(STARTER_ROW_X, ry, STARTER_ROW_W, STARTER_ROW_H, 6, ink);
    }

    /* The sprite keeps its own colours on the selected row: the alternative flag
     * drawMap() takes is a black silhouette, which is what disappears against a
     * dark inverted fill. Colour reads on either background. */
    if (fi >= 0)
      drawMap(SPECIES[fi].sprite, SPRITE_H, STARTER_ROW_X + 4, ry + 4, 1, false);
    gfx->setTextSize(1);
    gfx->setTextColor(sel ? (gNight ? UI_BG_NIGHT : UI_BG_DAY) : ink);
    gfx->setCursor(STARTER_ROW_X + 50, ry + STARTER_ROW_H/2 - 4);
    gfx->print(DEX_TBL[CLASSIC_DEX[i]].name);
  }
  gfx->setTextColor(ink);
}

static void renderCard() {
  gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);
  uint16_t ink = inkColor();
  const char *name = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  gfx->setCursor(4, 2);
  gfx->print(name);
  char buf[32];
  snprintf(buf, sizeof(buf), "Nv.%u", pet.level());
  gfx->setCursor(GFX_WIDTH - 40, 2);
  gfx->print(buf);
  int8_t fi = flashIdxForDex(pet.speciesId);
  if (fi >= 0)
    drawMap(SPECIES[fi].sprite, SPRITE_H, TP_CX - 32, 16, 2, false);
  gfx->setCursor(4, 90);
  gfx->printf("ATK %u  DEF %u  SPE %u", pet.atkStat(), pet.defStat(), pet.speStat());
  gfx->setCursor(4, 104);
  gfx->printf("WGT %u  BOND %u", pet.weight, pet.bond);
  gfx->setCursor(4, 120);
  gfx->printf(T(S_MEDALS_FMT), pet.totalMedals, MED_COUNT);
  centerTextBottom(T(S_BACK), TP_CX, 8, 1);
}

static void drawThumb(int16_t dex, int16_t x, int16_t y, uint8_t s, bool sil) {
  /* Prefer the SD thumbnail pack; fall back to the flash sprite if absent. */
  const uint8_t *thumb = thumbs.get(dex);
  if (thumb) {
    /* 24x24 raw bitmap, scaled by s. Each byte is a palette index into the
     * sprite palette; for thumbnails we just plot a solid cell. */
    for (uint8_t r = 0; r < 24; r++)
      for (uint8_t c = 0; c < 24; c++) {
        uint8_t px = thumb[r * 24 + c];
        if (px == 0) continue;
        uint16_t col = sil ? INK_K : spriteColor((char)px);
        gfx->fillRect(x + c * s, y + r * s, s, s, col);
      }
    return;
  }
  int8_t fi = flashIdxForDex(dex);
  if (fi >= 0 && fi < NUM_SPECIES) {
    drawMap(SPECIES[fi].sprite, SPRITE_H, x, y, s, sil);
  } else {
    gfx->fillRect(x, y, 24, 24, UI_TRACK);
    centerText("?", x + 12, y + 4, 2);
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
      drawThumb(dex, TP_CX - 12, GAL_DET_SPRITE_CY - 12, 1, !reg);
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
    uint16_t fill = (focus == i) ? UI_BAR_WARN
                                 : (reg ? UI_WHITE : UI_TRACK);
    gfx->fillRoundRect(x, y, GAL_CELL - GAL_GAP, GAL_CELL - GAL_GAP, 4, fill);
    gfx->drawRoundRect(x, y, GAL_CELL - GAL_GAP, GAL_CELL - GAL_GAP, 4, ink);
    if (reg) {
      drawThumb(dex, x + 6, y + 6, 1, false);
      if (shiny) {
        gfx->setTextSize(1);
        gfx->setTextColor(UI_BAR_WARN);
        gfx->setCursor(x + GAL_CELL - 14, y + 2);
        gfx->print("*");
      }
    } else {
      char nb[4];
      snprintf(nb, sizeof(nb), "%u", dex);
      centerText(nb, x + (GAL_CELL - GAL_GAP) / 2,
                 y + (GAL_CELL - GAL_GAP) / 2 - GFX_GLYPH_H, 1);
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

  /* Name preview box */
  gfx->fillRoundRect(KB_NAME_X, KB_NAME_Y, KB_NAME_W, KB_NAME_H, KB_NAME_R, UI_TRACK);
  gfx->drawRoundRect(KB_NAME_X, KB_NAME_Y, KB_NAME_W, KB_NAME_H, KB_NAME_R, ink);
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
    uint16_t fill = focused ? UI_BAR_WARN
                            : (special ? UI_TRACK : UI_WHITE);
    uint16_t fg   = focused ? UI_WHITE
                            : (special ? UI_BAR_WARN : ink);
    gfx->fillRoundRect(x + 2, y + 2, KB_W - 4, KB_H - 4, KB_R, fill);
    gfx->drawRoundRect(x + 2, y + 2, KB_W - 4, KB_H - 4, KB_R, ink);
    const char *label;
    char buf[2];
    if (i == KB_SPECIAL0) label = "<";
    else if (i == KB_SPECIAL1) label = "OK";
    else { buf[0] = KB_KEYS[i]; buf[1] = 0; label = buf; }
    int16_t lw = textWidth(label, KB_TEXT_SIZE);
    gfx->setTextSize(KB_TEXT_SIZE);
    gfx->setTextColor(fg);
    gfx->setCursor(x + KB_W / 2 - lw / 2,
                   y + KB_H / 2 - GFX_GLYPH_H * KB_TEXT_SIZE / 2);
    gfx->print(label);
  }
}

static void drawClockBtn(int16_t x, int16_t y, const char *label, bool focused) {
  uint16_t ink = inkColor();
  uint16_t fill = focused ? UI_BAR_WARN : UI_TRACK;
  uint16_t fg = focused ? UI_WHITE : ink;
  gfx->fillRoundRect(x, y, CLOCK_BTN_W, CLOCK_BTN_H, 4, fill);
  gfx->drawRoundRect(x, y, CLOCK_BTN_W, CLOCK_BTN_H, 4, ink);
  int16_t lw = textWidth(label, 2);
  gfx->setTextSize(2);
  gfx->setTextColor(fg);
  gfx->setCursor(x + CLOCK_BTN_W / 2 - lw / 2,
                 y + (CLOCK_BTN_H - 2 * GFX_GLYPH_H) / 2);
  gfx->print(label);
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

  /* Sound pill */
  bool ae = audioEnabled();
  gfx->fillRoundRect(CLOCK_SOUND_X, CLOCK_PILL_Y, CLOCK_SOUND_W, CLOCK_PILL_H,
                     4, focus == 4 ? UI_BAR_WARN : UI_TRACK);
  gfx->drawRoundRect(CLOCK_SOUND_X, CLOCK_PILL_Y, CLOCK_SOUND_W, CLOCK_PILL_H,
                     4, ink);
  {
    const char *s = ae ? T(S_SND_ON) : T(S_SND_OFF);
    gfx->setTextSize(1);
    gfx->setTextColor(focus == 4 ? UI_WHITE : ink);
    gfx->setCursor(CLOCK_SOUND_X + CLOCK_SOUND_W / 2 - textWidth(s, 1) / 2,
                   CLOCK_PILL_Y + (CLOCK_PILL_H - GFX_GLYPH_H) / 2);
    gfx->print(s);
  }

  /* Language pill */
  gfx->fillRoundRect(CLOCK_LANG_X, CLOCK_PILL_Y, CLOCK_LANG_W, CLOCK_PILL_H,
                     4, focus == 5 ? UI_BAR_WARN : UI_TRACK);
  gfx->drawRoundRect(CLOCK_LANG_X, CLOCK_PILL_Y, CLOCK_LANG_W, CLOCK_PILL_H,
                     4, ink);
  {
    /* One label per Lang, in enum order. The static_assert is the point: this
     * table is indexed by gLang, so adding a language without extending it
     * reads past the end and hands print() a wild pointer -- which is exactly
     * what adding Korean did. */
    const char *lng_name[] = {"ES", "EN", "FR", "DE", "IT", "PT", "KO"};
    static_assert(sizeof(lng_name) / sizeof(lng_name[0]) == LANG_COUNT,
                  "language label table must cover every Lang");
    const char *s = lng_name[(int)gLang];
    gfx->setTextSize(1);
    gfx->setTextColor(focus == 5 ? UI_WHITE : ink);
    gfx->setCursor(CLOCK_LANG_X + CLOCK_LANG_W / 2 - textWidth(s, 1) / 2,
                   CLOCK_PILL_Y + (CLOCK_PILL_H - GFX_GLYPH_H) / 2);
    gfx->print(s);
  }

  /* OK button */
  bool ok_focused = (focus == 6);
  gfx->fillRoundRect(CLOCK_OK_X, CLOCK_OK_Y, CLOCK_OK_W, CLOCK_OK_H, 4,
                     ok_focused ? UI_BAR_OK : UI_TRACK);
  gfx->drawRoundRect(CLOCK_OK_X, CLOCK_OK_Y, CLOCK_OK_W, CLOCK_OK_H, 4, ink);
  {
    const char *s = T(S_YES);
    int16_t lw = textWidth(s, CLOCK_OK_SIZE);
    gfx->setTextSize(CLOCK_OK_SIZE);
    gfx->setTextColor(ok_focused ? UI_WHITE : ink);
    gfx->setCursor(CLOCK_OK_X + CLOCK_OK_W / 2 - lw / 2,
                   CLOCK_OK_Y + (CLOCK_OK_H - CLOCK_OK_SIZE * GFX_GLYPH_H) / 2);
    gfx->print(s);
  }
}

static void renderGame() {
  gfx->fillScreen(C565(0x10, 0x18, 0x28));
  uint16_t ink = inkColor();
  char buf[16];
  snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
  centerText(buf, TP_CX, 4, 1);
  gfx->fillRect(0, GAME_TOP, GFX_WIDTH, 1, UI_TRACK);
  gfx->fillCircle((int16_t)ballX, (int16_t)ballY, GAME_BALL_R, UI_BAR_BAD);
  gfx->fillRect((int16_t)paddleX, GAME_PADDLE_Y, GAME_PADDLE_W, GAME_PADDLE_H, UI_WHITE);
}

static void renderSack() {
  uint32_t now = millis();
  uint16_t ink = inkColor();
  gfx->fillScreen(gNight ? UI_BG_NIGHT : UI_BG_DAY);

  /* Result screen: timed reveal of str gain / record. */
  if (now >= sackUntil && now < sackOverUntil) {
    char buf[24];
    snprintf(buf, sizeof(buf), T(S_HITS_FMT), sackHits);
    centerText(buf, TP_CX, SACK_RESULT_Y, SACK_RESULT_SIZE);
    snprintf(buf, sizeof(buf), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_BAD);
    centerText(buf, TP_CX, SACK_RESULT_Y + 28, 2);
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

  /* HUD */
  char buf[24];
  snprintf(buf, sizeof(buf), T(S_HITS_FMT), sackHits);
  centerText(buf, TP_CX, SACK_HIT_COUNTER_Y, SACK_HIT_COUNTER_SZ);
  centerText(T(S_HIT_FAST), TP_CX, SACK_HINT_Y, SACK_HINT_SIZE);

  /* time bar: full width at start, shrinks to zero as the timer runs out. */
  uint32_t total = 10000;
  uint32_t remain = (sackUntil > now) ? (sackUntil - now) : 0;
  uint16_t fillW = (uint16_t)((uint64_t)remain * SACK_BAR_W / total);
  gfx->fillRoundRect(SACK_BAR_X, SACK_BAR_Y, SACK_BAR_W, SACK_BAR_H, 4, UI_TRACK);
  gfx->fillRoundRect(SACK_BAR_X, SACK_BAR_Y, fillW, SACK_BAR_H, 4, UI_BAR_OK);
  gfx->drawRoundRect(SACK_BAR_X, SACK_BAR_Y, SACK_BAR_W, SACK_BAR_H, 4, ink);
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

  uint16_t panel = gNight ? PANEL_NIGHT : PANEL_DAY;
  gfx->fillRect(0, 152, GFX_WIDTH, GFX_HEIGHT - 152, panel);

  drawBars();
  drawButtons();
  drawCelebration();

  if (!pet.evolving() && pet.wantEvolveButton()) {
    choiceKind = 1;
    choiceUntil = now + 15000;
  }
  if (!pet.evolving() && pet.wantFarewellButton()) {
    choiceKind = 2;
    choiceUntil = now + 15000;
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

  switch (current_screen_id()) {
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

void tamapoke_ui_goto_screen(int id) {
  harness_screen = (int8_t)id;
  uint32_t now = millis();

  /* Clear all transient state so each capture is reproducible. */
  galleryDetail = 0;
  cardPage = 0;
  galleryOpen = cardOpen = kbOpen = clockOpen = gameOpen = sackOpen = false;
  if (galleryPmd.loaded) galleryPmd.unload();
  bathUntil = feedMenuUntil = confirmUntil = choiceUntil = 0;

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
      gameScore = 3; gameMisses = 1;
      break;
    case 7: /* sack */
      sackOpen = true;
      sackUntil = now + 10000;
      sackHits = 24; sackGain = 2; sackShake = 0;
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
      feedMenuUntil = now + 3000;
      break;
    case 10: /* release-confirm overlay */
      harness_screen = 0;
      if (pet.isEgg() || pet.speciesId < 1) {
        pet.chooseStarter(CLASSIC_DEX[0]);
        pet.speciesId = CLASSIC_DEX[0];
      }
      ensureMon();
      confirmUntil = now + 10000;
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

  if (gameOpen && !pet.evolving()) {
    ballX += ballVX;
    ballY += ballVY;
    if (ballX < 4 || ballX > GFX_WIDTH - 4) ballVX = -ballVX;
    if (ballY < GAME_TOP) ballVY = -ballVY;
    if (ballY >= GAME_PADDLE_Y) {
      if (ballX >= paddleX && ballX <= paddleX + GAME_PADDLE_W) {
        ballVY = -fabsf(ballVY);
        ballVX += (ballX - paddleX - GAME_PADDLE_W/2) * 0.1f;
        gameScore++;
      } else {
        gameMisses++;
        ballY = GAME_TOP + 20;
        ballVX = random(-3, 4);
        ballVY = random(2, 5);
      }
    }
  }

  if (now_ms - lastInteract > 30000) dimStage = 1;
  if (now_ms - lastInteract > 60000) dimStage = 2;

  tamapoke_ui_render();
}
