#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "odroid_system.h"
#include "odroid_settings.h"
#include "main.h"
#include "rg_i18n.h"
#include "rg_storage.h"
#include "appid.h"
#include "gui.h"
#include "favorites.h"
#include "rom_manager.h"
#include "gw_sdcard.h"
#include "gw_ofw.h"

#define CONFIG_MAGIC 0xcafef00d
#define ODROID_APPID_COUNT 4

/* Favorites: the in-RAM favorites[32] hash array (128 B in DTCM-resident
 * persistent_config) was REMOVED (2026-06-29) to reclaim DTCM for emulator
 * cores, and the favorites UI/accessors were removed entirely afterwards.
 * Only the game-list sort mode persists in persistent_config now. */

#if !defined  (COVERFLOW)
  #define COVERFLOW 0
#endif /* COVERFLOW */
// Global
#if !defined (CHEAT_CODES)
#define CHEAT_CODES 0
#endif
#if !defined (CODEPAGE)
#define CODEPAGE 1252
#endif
#if !defined (UICODEPAGE)
#define UICODEPAGE 1252
#endif
static const char* Key_RomFilePath  = "RomFilePath";
static const char* Key_AudioSink    = "AudioSink";

// Per-app
static const char* Key_DispRotation = "DistRotation";

typedef struct app_config {
    uint8_t region;
    uint8_t palette;
    uint8_t disp_scaling;
    uint8_t disp_filter;
    uint8_t disp_overscan;
    uint8_t sprite_limit;
} app_config_t;

#if CHEAT_CODES == 1
typedef struct {
    char game_path[RG_PATH_MAX + 1];
    uint32_t active_cheat_codes;
    bool is_cached;
} CheatCache;

static CheatCache cheat_cache = {{0}, 0, false};
#endif

typedef struct persistent_config {
    uint32_t magic;
    uint8_t version;

    uint8_t backlight;
    uint8_t start_action;
    uint8_t volume;
    uint8_t font_size;
    uint8_t theme;
    uint8_t colors;
    uint8_t turbo_buttons;
    uint8_t font;
    uint8_t lang;
    uint8_t startup_app;
    uint8_t cpu_oc_level;
    char    startup_file[256];

    uint16_t main_menu_timeout_s;
    uint16_t main_menu_selected_tab;
    uint16_t main_menu_cursor;
    char main_menu_browse_subpath[96];

    bool debug_clock_always_on;

    /** Launcher cover style (odroid_cover_style_t). Occupies what used to be
     * the single alignment-padding byte between debug_clock_always_on and the
     * 4-aligned welcome_prompt — struct size and every other field offset are
     * unchanged, so existing v14 /CONFIG files load as-is (the padding byte in
     * files written by older builds is 0 = poster, today's behavior). NO
     * version bump needed. If another field is ever added here, that free
     * lunch is gone: re-check offsets and bump the version. */
    uint8_t cover_style;

    /** Welcome prompt: 0 = not anchored, 1 = message already shown, else YYYYMMDD anchor (RTC >= 2026). */
    uint32_t welcome_prompt;

    /* favorites[32] removed — reclaimed 128 B DTCM (feature was RAM-resident). */
    /** Game list sort mode (odroid_sort_mode_t). */
    uint8_t sort_mode;

    app_config_t app[APPID_COUNT];

    uint32_t crc32;
} persistent_config_t;

/* cover_style must live entirely inside the padding that already existed
 * before welcome_prompt: welcome_prompt has to sit exactly where a build
 * WITHOUT cover_style put it, or every saved /CONFIG shifts and silently
 * misreads. If this fires, the byte is no longer free — move the field to
 * the end and bump the version instead. */
_Static_assert(offsetof(persistent_config_t, welcome_prompt) ==
                   ((offsetof(persistent_config_t, debug_clock_always_on) + 1 + 3) / 4) * 4,
               "cover_style must fit in pre-existing padding before welcome_prompt");

static const persistent_config_t persistent_config_default = {
    .magic = CONFIG_MAGIC,
    .version = 18,  /* 13->14: APPID_32X grows app[APPID_COUNT] (one-time settings reset on upgrade)
                     * 14->15: APPID_CPS1 was added sharing APPID_SM's slot 23.
                     * 15->16: APPID_CPS1 moved to its own slot 28, growing the struct.
                     * 16->17: APPID_CPS1 and APPID_SEGACD removed, shrinking the struct.
                     * 17->18: APPID_SEGACD returns in its old slot 27, growing it back
                     * (gate-6 forward port, docs/SEGACD_REASSESSMENT_2026-09-14.md).
                     * Every user loses language, coverflow, backlight and volume ONCE
                     * on this upgrade (CLAUDE.md). */

    .backlight = ODROID_BACKLIGHT_LEVEL6,
    .start_action = ODROID_START_ACTION_RESUME,
    .volume = ODROID_AUDIO_VOLUME_MAX / 2, // Too high volume can cause brown out if the battery isn't connected.
    .font_size = 8,
    .theme = 2, //use as theme index
    .colors = 0,
    .turbo_buttons = 0,
    .font = 0,
#if CODEPAGE==12521
    .lang = 1,
#elif CODEPAGE==12522
    .lang = 2,
#elif CODEPAGE==12523
    .lang = 3,
#elif CODEPAGE==12524
    .lang = 4,
#elif CODEPAGE==12525
    .lang = 5,
#elif CODEPAGE==12526
    .lang = 6,
#elif CODEPAGE==12511
    .lang = 7,
#elif CODEPAGE==932
    .lang = 11,
#elif CODEPAGE==936
    .lang = 8,
#elif CODEPAGE==949
    .lang = 10,
#elif CODEPAGE==950
    .lang = 9,
#else
    .lang = 0,
#endif
    .startup_app = 0,
    .cpu_oc_level = 0,
    .main_menu_timeout_s = 60 * 10, // Turn off after 10 minutes of idle time in the main menu
    .main_menu_selected_tab = 0,
    .main_menu_cursor = 0,
    .main_menu_browse_subpath = {0},
    .debug_clock_always_on = false,
    .cover_style = ODROID_COVER_STYLE_POSTER,
    .welcome_prompt = 0,
    .sort_mode = 0,
    .app = {
        {0}, // Launcher
        {
            .region = 0,
            .palette = 2,
            .disp_scaling = ODROID_DISPLAY_SCALING_FULL,
            .disp_filter = ODROID_DISPLAY_FILTER_SHARP,
            .disp_overscan = 0,
            .sprite_limit = 0,
        }, // GB
        {
            .disp_scaling = ODROID_DISPLAY_SCALING_CUSTOM,
            .disp_filter = ODROID_DISPLAY_FILTER_SHARP,
        }, // NES
        {0}, // SMS
        {0}, // PCE
        {0}, // GW
        {0}, // MD Genesis
    },
};

persistent_config_t persistent_config_ram;

static bool file_exists(const char *file_path) {
    struct stat st;
    return stat(file_path, &st) == 0;
}

void odroid_settings_init()
{
    if (fs_mounted && file_exists("/CONFIG")) {
        FILE *file = fopen("/CONFIG", "rb");
        if (file) {
            size_t bytes_read = fread((unsigned char *)&persistent_config_ram, 1, sizeof(persistent_config_t), file);
            fclose(file);
            if (bytes_read != sizeof(persistent_config_t))
                memset(&persistent_config_ram, 0, sizeof(persistent_config_t));
        }
    }

    if (persistent_config_ram.magic != CONFIG_MAGIC) {
        printf("CONFIG magic %08x/%08lx\n", CONFIG_MAGIC, persistent_config_ram.magic);
        odroid_settings_reset();
        return;
    }

    if (persistent_config_ram.version != persistent_config_default.version) {
        printf("CONFIG ver reset\n");
        odroid_settings_reset();
        return;
    }

    // Calculate crc32 of the whole struct with the crc32 value set to 0
    uint32_t loaded_crc32 = persistent_config_ram.crc32;
    persistent_config_ram.crc32 = 0;
    persistent_config_ram.crc32 = crc32_le(0, (unsigned char *) &persistent_config_ram, sizeof(persistent_config_t));

    if (persistent_config_ram.crc32 != loaded_crc32) {
        printf("Config: CRC32 mismatch. Expected 0x%08lx, got 0x%08lx\n", persistent_config_ram.crc32, loaded_crc32);
        odroid_settings_reset();
        return;
    }
    //set colors;
    curr_colors = (colors_t *)(&gui_colors[persistent_config_ram.colors]);
    gui_apply_colors_to_overlay_clut();
    //set font
    set_font(odroid_settings_font_get());
    //set lang — load strings from /lang/xx_xx.bin on SD, fall back to
    //  baked lang_en_us if the file is missing or corrupt.
    curr_lang = i18n_load_language(odroid_settings_lang_get());
}

void odroid_settings_commit()
{
    // Calculate crc32 of the whole struct with the crc32 value set to 0
    persistent_config_ram.crc32 = 0;
    persistent_config_ram.crc32 = crc32_le(0, (unsigned char *) &persistent_config_ram, sizeof(persistent_config_t));

    if (fs_mounted) {
        FILE *file = fopen("/CONFIG", "wb");
        if (file) {
            fwrite((const void *)&persistent_config_ram, 1, sizeof(persistent_config_t), file);
            fclose(file);
        }
    }

    /* Settings dialog has just closed. Re-load the committed language
     * (no-op / already active when the user left the picker on the
     * same idx; otherwise swaps the single SD language slot). */
    curr_lang = i18n_load_language(odroid_settings_lang_get());
}

#if CHEAT_CODES == 1
static int delete_cheat_state_file_cb(const rg_scandir_t *file, void *arg)
{
    (void)arg;

    if (file->is_file) {
        const char *ext = strrchr(file->basename, '.');
        if (ext && strcmp(ext, ".state") == 0)
            rg_storage_delete(file->path);
    }

    return RG_SCANDIR_CONTINUE;
}

static void odroid_settings_delete_cheat_state_files()
{
    rg_storage_scandir(ODROID_BASE_PATH_SAVES, delete_cheat_state_file_cb, NULL,
                       RG_SCANDIR_FILES | RG_SCANDIR_RECURSIVE);
}
#endif

void odroid_settings_reset()
{
#if CHEAT_CODES == 1
    // Delete all cheat state files
    odroid_settings_delete_cheat_state_files();
    // Reset cheat cache
    cheat_cache.is_cached = false;
    cheat_cache.game_path[0] = '\0';
    cheat_cache.active_cheat_codes = 0;
#endif
    memcpy(&persistent_config_ram, &persistent_config_default, sizeof(persistent_config_t));

    odroid_settings_commit();
}

char* odroid_settings_string_get(const char *key, const char *default_value)
{
    return (char *) default_value;
}

void odroid_settings_string_set(const char *key, const char *value)
{
}

int32_t odroid_settings_int32_get(const char *key, int32_t default_value)
{
    return default_value;
}

void odroid_settings_int32_set(const char *key, int32_t value)
{
}

void odroid_settings_cpu_oc_level_set(uint8_t oc)
{
    oc = (oc < 0) ? 0 : ((oc > 2) ? 2 : oc);
    persistent_config_ram.cpu_oc_level = oc;
}

uint8_t odroid_settings_cpu_oc_level_get(void)
{
    return persistent_config_ram.cpu_oc_level;
}

int8_t odroid_settings_colors_get()
{
    int colors = persistent_config_ram.colors;
    if (colors < 0)
        persistent_config_ram.colors = 0;
    else if (colors >= gui_colors_count)
        persistent_config_ram.colors = gui_colors_count - 1;
    return persistent_config_ram.colors;
}

void odroid_settings_colors_set(int8_t colors)
{
    if (colors < 0)
        colors = 0;
    else if (colors >= gui_colors_count)
        colors = gui_colors_count - 1;
    persistent_config_ram.colors = colors;
}


int8_t odroid_settings_font_get()
{
    int font = persistent_config_ram.font;
    if (font < 0)
        persistent_config_ram.font = 0;
    else if (font >= gui_font_count)
        persistent_config_ram.font = gui_font_count - 1;
    return persistent_config_ram.font;
}

void odroid_settings_font_set(int8_t font)
{
    if (font < 0)
        font = 0;
    else if (font >= gui_font_count)
        font = gui_font_count - 1;
    persistent_config_ram.font = font;
}

int8_t odroid_settings_turbo_buttons_get()
{
    int turbo_buttons = persistent_config_ram.turbo_buttons;
    if (turbo_buttons < 0)
        persistent_config_ram.turbo_buttons = 0;
    else if (turbo_buttons >= 3)
        persistent_config_ram.turbo_buttons = 3;
    return persistent_config_ram.turbo_buttons;
}

void odroid_settings_turbo_buttons_set(int8_t turbo_buttons)
{
    if (turbo_buttons < 0)
        turbo_buttons = 0;
    else if (turbo_buttons >= 3)
        turbo_buttons = 3;
    persistent_config_ram.turbo_buttons = turbo_buttons;
}


int8_t odroid_settings_get_next_lang(uint8_t cur)
{
    /* gui_lang[] removed — all entries were non-NULL (only present-when-
     * INCLUDED languages were in the array), so the original while-loop
     * always exited on its first iteration. Reduce to simple wrap. */
    int ret = cur + 1;
    if (ret >= gui_lang_count)
        ret = 0;
    return ret;
}

int8_t odroid_settings_get_prior_lang(uint8_t cur)
{
    int ret = (int)cur - 1;
    if (ret < 0)
        ret = gui_lang_count - 1;
    return ret;
}

int8_t odroid_settings_lang_get()
{
    int lang = persistent_config_ram.lang;
    if(lang >= gui_lang_count){
        // This can happen if a language is set, then the device is reflashed with fewer languages.
        lang = 0;
    }
    return odroid_settings_get_prior_lang(lang + 1);
}


void odroid_settings_lang_set(int8_t lang)
{
    if (lang < 0)
        lang = 0;
    else if (lang >= gui_lang_count)
        lang = gui_lang_count - 1;
    persistent_config_ram.lang = lang;
}

#if COVERFLOW != 0
int8_t odroid_settings_theme_get()
{
    int theme = persistent_config_ram.theme;
    if (theme < 0)
        persistent_config_ram.theme = 0;
    else if (theme > 4)
        persistent_config_ram.theme = 4;
    return persistent_config_ram.theme;
}
void odroid_settings_theme_set(int8_t theme)
{
    if (theme < 0)
        theme = 0;
    else if (theme > 4)
        theme = 4;
    persistent_config_ram.theme = theme;
}
#else
int8_t odroid_settings_theme_get() { return 0; }
void odroid_settings_theme_set(int8_t theme) { (void)theme; }
#endif


int32_t odroid_settings_app_int32_get(const char *key, int32_t default_value)
{
    return default_value;
}

void odroid_settings_app_int32_set(const char *key, int32_t value)
{
    char app_key[16];
    sprintf(app_key, "%.12s.%ld", key, odroid_system_get_app()->id);
    odroid_settings_int32_set(app_key, value);
}

/* ---- Per-emulator direct button mapping ---------------------------------
 *
 * Only the currently running core's eight-byte map is resident.  Persisting
 * one tiny file per APPID avoids adding APPID_COUNT * N bytes to the DTCM
 * configuration object, whose layout/version is deliberately expensive to
 * change on this target. */
#define KEYMAP_MAGIC   0x50414d4bu /* "KMAP" little-endian */
#define KEYMAP_VERSION 1u

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t app_id;
    uint8_t count;
    uint8_t map[ODROID_KEYMAP_MAX_ACTIONS];
} keymap_file_t;

typedef struct {
    uint8_t app_id;
    uint8_t count;
    const char *const *names;
    const uint8_t *defaults;
} keymap_profile_t;

static const uint8_t keymap_nes_defaults[] = {
    ODROID_INPUT_A, ODROID_INPUT_B, ODROID_INPUT_START, ODROID_INPUT_SELECT
};
static const char *const keymap_nes_names[] = { "A", "B", "Start", "Select" };

static const uint8_t keymap_snes_defaults[] = {
    ODROID_INPUT_B, ODROID_INPUT_Y, ODROID_INPUT_SELECT, ODROID_INPUT_START,
    ODROID_INPUT_A, ODROID_INPUT_X, ODROID_KEYMAP_OFF, ODROID_KEYMAP_OFF
};
static const char *const keymap_snes_names[] = {
    "B", "Y", "Select", "Start", "A", "X", "L", "R"
};

static const uint8_t keymap_md_defaults[] = {
    ODROID_INPUT_A, ODROID_INPUT_B, ODROID_INPUT_SELECT,
    ODROID_KEYMAP_OFF, ODROID_KEYMAP_OFF, ODROID_KEYMAP_OFF,
    ODROID_KEYMAP_OFF, ODROID_INPUT_START
};
static const char *const keymap_md_names[] = {
    "A", "B", "C", "X", "Y", "Z", "Mode", "Start"
};

/* GBA defaults keep the legacy hard wiring (X->R, Y->L) on both units; the
 * point of the keymap is that the user can re-point L/R anywhere except
 * POWER when the default twists fingers on their unit. */
static const uint8_t keymap_gba_defaults[] = {
    ODROID_INPUT_A, ODROID_INPUT_B, ODROID_INPUT_Y, ODROID_INPUT_X,
    ODROID_INPUT_START, ODROID_INPUT_SELECT
};
static const char *const keymap_gba_names[] = { "A", "B", "L", "R", "Start", "Select" };

static const keymap_profile_t keymap_profiles[] = {
    { APPID_NES,    4, keymap_nes_names,  keymap_nes_defaults  },
    { APPID_SNES,   8, keymap_snes_names, keymap_snes_defaults },
    { APPID_MD,     8, keymap_md_names,   keymap_md_defaults   },
    { APPID_GBA,    6, keymap_gba_names,  keymap_gba_defaults  },
    /* Sega CD plays with the exact Genesis pad, so it shares the MD action
     * set -- and the MD's saved file (see keymap_canonical_id): one
     * /KEYMAP-<MD> serves both cores, so a pad tuned in Sonic 2 carries
     * into Sonic CD unchanged. */
    { APPID_SEGACD, 8, keymap_md_names,   keymap_md_defaults   },
};

/* Cores that are physically the same pad share one saved file.  The header's
 * app_id is written with this canonical id, so validation accepts the file
 * from either core. */
static uint8_t keymap_canonical_id(uint8_t app_id)
{
    return app_id == APPID_SEGACD ? APPID_MD : app_id;
}

static uint8_t keymap_loaded_app = 0xff;
static uint8_t keymap_count;
static uint8_t keymap_current[ODROID_KEYMAP_MAX_ACTIONS];

static const keymap_profile_t *keymap_profile(void)
{
    uint8_t app_id = (uint8_t)odroid_system_get_app()->id;
    for (unsigned i = 0; i < sizeof(keymap_profiles) / sizeof(keymap_profiles[0]); i++)
        if (keymap_profiles[i].app_id == app_id)
            return &keymap_profiles[i];
    return NULL;
}

static bool keymap_physical_valid(uint8_t key, uint8_t app_id)
{
    if (key == ODROID_KEYMAP_OFF || key == ODROID_INPUT_A || key == ODROID_INPUT_B)
        return true;
    if (!get_ofw_is_mario()) {
        /* Zelda unit: A/B, the console keys (GAME/TIME) and the labelled
         * face START/SELECT (X/Y) are all real, game-usable physicals.
         * PAUSE stays excluded -- it is the only pause/set button. */
        return key == ODROID_INPUT_START || key == ODROID_INPUT_SELECT ||
               key == ODROID_INPUT_X || key == ODROID_INPUT_Y;
    }
    /* Mario unit has no X/Y physicals at all.  On the plain cores PAUSE
     * (VOLUME) is the menu-open key in common_emu_input_loop, so it cannot
     * serve a game action; the MD-family cores swap TIME/PAUSE instead
     * (gwenesis TIME<->PAUSE swap), making PAUSE the game key and TIME
     * (SELECT) the menu key. */
    if (app_id == APPID_MD || app_id == APPID_SEGACD)
        return key == ODROID_INPUT_START || key == ODROID_INPUT_VOLUME;
    return key == ODROID_INPUT_START || key == ODROID_INPUT_SELECT;
}

static void keymap_copy_defaults(const keymap_profile_t *profile, uint8_t *map)
{
    memcpy(map, profile->defaults, profile->count);
    /* Gwenesis swaps TIME and PAUSE/SET for the Mario hardware.  Preserve its
     * established usable C-button default while the Zelda unit can use its
     * dedicated START key. */
    if (profile->app_id == APPID_MD || profile->app_id == APPID_SEGACD)
        map[ODROID_KEYMAP_MD_C] = get_ofw_is_mario() ? ODROID_INPUT_VOLUME
                                                     : ODROID_INPUT_X;
    /* The legacy NES cores accepted START/SELECT (GAME/TIME) AND the Zelda
     * unit's labelled START/SELECT (X/Y) for the same action; a single-mapping
     * keymap picks per model.  NES only: the SNES profile's base defaults
     * already give Y the labelled SELECT and X the labelled START while
     * Select/Start keep TIME/GAME -- overriding them per model would
     * double-book the physicals (2026-09-20 review).  MD keeps START on
     * both models, matching the old gwenesis behaviour. */
    if (!get_ofw_is_mario() && profile->app_id == APPID_NES) {
        map[ODROID_KEYMAP_NES_START] = ODROID_INPUT_X;
        map[ODROID_KEYMAP_NES_SELECT] = ODROID_INPUT_Y;
    }
}

static void keymap_path(char *path, size_t size, uint8_t app_id)
{
    snprintf(path, size, "/KEYMAP-%02u", (unsigned)app_id);
}

static void keymap_ensure_loaded(void)
{
    const keymap_profile_t *profile = keymap_profile();
    uint8_t app_id = profile ? profile->app_id : 0xff;
    if (keymap_loaded_app == app_id)
        return;

    keymap_loaded_app = app_id;
    keymap_count = profile ? profile->count : 0;
    memset(keymap_current, ODROID_KEYMAP_OFF, sizeof(keymap_current));
    if (!profile)
        return;
    keymap_copy_defaults(profile, keymap_current);

    char path[20];
    keymap_file_t saved;
    keymap_path(path, sizeof(path), keymap_canonical_id(app_id));
    FILE *file = fopen(path, "rb");
    if (!file)
        return;
    size_t got = fread(&saved, 1, sizeof(saved), file);
    fclose(file);
    if (got != sizeof(saved) || saved.magic != KEYMAP_MAGIC ||
        saved.version != KEYMAP_VERSION ||
        saved.app_id != keymap_canonical_id(app_id) ||
        saved.count != profile->count)
        return;
    /* Safe normalisation, not rejection: a file written on the other unit
     * model (or a future policy change) may carry physicals this model/core
     * cannot use -- each such entry falls back to that action's default
     * instead of discarding the whole file (2026-09-20 review). */
    for (int i = 0; i < saved.count; i++)
        if (!keymap_physical_valid(saved.map[i], app_id))
            saved.map[i] = keymap_current[i];
    memcpy(keymap_current, saved.map, saved.count);
}

static void keymap_save(void)
{
    const keymap_profile_t *profile = keymap_profile();
    if (!profile || !fs_mounted)
        return;
    keymap_file_t saved = {
        .magic = KEYMAP_MAGIC, .version = KEYMAP_VERSION,
        .app_id = keymap_canonical_id(profile->app_id),
        .count = profile->count,
    };
    memset(saved.map, ODROID_KEYMAP_OFF, sizeof(saved.map));
    memcpy(saved.map, keymap_current, profile->count);
    char path[20];
    keymap_path(path, sizeof(path), keymap_canonical_id(profile->app_id));
    FILE *file = fopen(path, "wb");
    if (!file || fwrite(&saved, 1, sizeof(saved), file) != sizeof(saved)) {
        /* A failed save must not look like a success -- leave the breadcrumb
         * in the persistent log (2026-09-20 review). */
        printf("keymap: save failed (%s)\n", path);
    }
    if (file)
        fclose(file);
}

bool odroid_keymap_supported(void)
{
    return keymap_profile() != NULL;
}

int odroid_keymap_action_count(void)
{
    keymap_ensure_loaded();
    return keymap_count;
}

const char *odroid_keymap_action_name(int action)
{
    const keymap_profile_t *profile = keymap_profile();
    return (profile && action >= 0 && action < profile->count) ? profile->names[action] : "?";
}

uint8_t odroid_keymap_get(int action)
{
    keymap_ensure_loaded();
    return (action >= 0 && action < keymap_count) ? keymap_current[action] : ODROID_KEYMAP_OFF;
}

void odroid_keymap_set(int action, uint8_t physical_key)
{
    keymap_ensure_loaded();
    if (action < 0 || action >= keymap_count ||
        !keymap_physical_valid(physical_key, keymap_profile() ? keymap_profile()->app_id : 0xff))
        return;
    keymap_current[action] = physical_key;
    /* No SD write here: the Controls dialog steps this on every event, so the
     * overlay commits once when the dialog closes (odroid_keymap_save). */
}

void odroid_keymap_save(void)
{
    if (keymap_loaded_app == 0xff)
        return;
    keymap_save();
}

void odroid_keymap_reset(void)
{
    const keymap_profile_t *profile = keymap_profile();
    keymap_ensure_loaded();
    if (!profile)
        return;
    keymap_copy_defaults(profile, keymap_current);
    /* Saved on dialog close, same as odroid_keymap_set. */
}

bool odroid_keymap_is_default(void)
{
    const keymap_profile_t *profile = keymap_profile();
    uint8_t defaults[ODROID_KEYMAP_MAX_ACTIONS];
    keymap_ensure_loaded();
    if (!profile)
        return false;
    keymap_copy_defaults(profile, defaults);
    return memcmp(keymap_current, defaults, profile->count) == 0;
}

bool odroid_keymap_pressed(const odroid_gamepad_state_t *pad, int action)
{
    uint8_t key = odroid_keymap_get(action);
    return pad && key != ODROID_KEYMAP_OFF && key < ODROID_INPUT_MAX && pad->values[key];
}

const char *odroid_keymap_physical_name(uint8_t key)
{
    switch (key) {
    case ODROID_INPUT_A:      return "A";
    case ODROID_INPUT_B:      return "B";
    case ODROID_INPUT_START:  return "GAME";
    case ODROID_INPUT_SELECT: return "TIME";
    case ODROID_INPUT_X:      return "START";
    case ODROID_INPUT_Y:      return "SELECT";
    case ODROID_INPUT_VOLUME: return "PAUSE";
    default:                  return "Off";
    }
}

uint8_t odroid_keymap_physical_step(uint8_t key, int direction)
{
    /* Offer exactly the keys keymap_physical_valid() admits for this unit
     * model and core -- the lists stay honest to the physical hardware
     * (no X/Y on Mario, no menu-reserved PAUSE on plain cores) instead of
     * a global everything-but-POWER list (2026-09-20 review). */
    const keymap_profile_t *profile = keymap_profile();
    uint8_t app_id = profile ? profile->app_id : 0xff;
    static const uint8_t candidates[] = {
        ODROID_KEYMAP_OFF, ODROID_INPUT_A, ODROID_INPUT_B,
        ODROID_INPUT_START, ODROID_INPUT_SELECT, ODROID_INPUT_X,
        ODROID_INPUT_Y, ODROID_INPUT_VOLUME
    };
    uint8_t choices[sizeof(candidates)];
    int count = 0;
    for (size_t i = 0; i < sizeof(candidates); i++)
        if (keymap_physical_valid(candidates[i], app_id))
            choices[count++] = candidates[i];
    if (count == 0)
        return ODROID_KEYMAP_OFF;
    int index = 0;
    for (int i = 0; i < count; i++)
        if (choices[i] == key) { index = i; break; }
    index = (index + (direction < 0 ? -1 : 1) + count) % count;
    return choices[index];
}

int odroid_keymap_conflict(int action)
{
    /* User policy: duplicate mappings are allowed -- freedom first -- but
     * they must be visible.  Returns the index of another action that shares
     * this action's physical key, or -1.  OFF never conflicts. */
    keymap_ensure_loaded();
    if (action < 0 || action >= keymap_count)
        return -1;
    uint8_t key = keymap_current[action];
    if (key == ODROID_KEYMAP_OFF)
        return -1;
    for (int i = 0; i < keymap_count; i++)
        if (i != action && keymap_current[i] == key)
            return i;
    return -1;
}


int32_t odroid_settings_FontSize_get()
{
    return persistent_config_ram.font_size;
}
void odroid_settings_FontSize_set(int32_t value)
{
    persistent_config_ram.font_size = value;
}

/*
char* odroid_settings_RomFilePath_get()
{
    static char filepath_buffer[FS_MAX_PATH_SIZE];  // Being static is fine since the name is immediately used.
    snprintf(filepath_buffer,
             sizeof(filepath_buffer),
             "%s/%s.savestate",
             ACTIVE_FILE->system->system_name,
             ACTIVE_FILE->name);
    return filepath_buffer;
}
*/
void odroid_settings_RomFilePath_set(const char* value)
{
  odroid_settings_string_set(Key_RomFilePath, value);
}


int32_t odroid_settings_Volume_get()
{
    return persistent_config_ram.volume;
}
void odroid_settings_Volume_set(int32_t value)
{
    persistent_config_ram.volume = value;
}


int32_t odroid_settings_AudioSink_get()
{
  return odroid_settings_int32_get(Key_AudioSink, ODROID_AUDIO_SINK_SPEAKER);
}
void odroid_settings_AudioSink_set(int32_t value)
{
  odroid_settings_int32_set(Key_AudioSink, value);
}



int32_t odroid_settings_Backlight_get()
{
    return persistent_config_ram.backlight;
}
void odroid_settings_Backlight_set(int32_t value)
{
    persistent_config_ram.backlight = value;
}


ODROID_START_ACTION odroid_settings_StartAction_get()
{
    return persistent_config_ram.start_action;
}
void odroid_settings_StartAction_set(ODROID_START_ACTION value)
{
    persistent_config_ram.start_action = value;
}


int32_t odroid_settings_StartupApp_get()
{
    return persistent_config_ram.startup_app;
}
void odroid_settings_StartupApp_set(int32_t value)
{
    persistent_config_ram.startup_app = value;
}


char* odroid_settings_StartupFile_get()
{
    return persistent_config_ram.startup_file;
}
void odroid_settings_StartupFile_set(retro_emulator_file_t *file)
{
    // We save only file path and we'll try to find it in built list at next startup
    if (file)
    {
        memcpy(&persistent_config_ram.startup_file, file->path, sizeof(persistent_config_ram.startup_file));
    }
    else
    {
        memset(&persistent_config_ram.startup_file,0,sizeof(persistent_config_ram.startup_file));
    }
}

uint16_t odroid_settings_MainMenuTimeoutS_get()
{
    return ((MIN(persistent_config_ram.main_menu_timeout_s, 3600) + 59) / 60) * 60; // > 0 : Round to whole minutes max one hour
}
void odroid_settings_MainMenuTimeoutS_set(uint16_t value)
{
    persistent_config_ram.main_menu_timeout_s = value;
}

uint16_t odroid_settings_MainMenuSelectedTab_get()
{
    return persistent_config_ram.main_menu_selected_tab;
}
void odroid_settings_MainMenuSelectedTab_set(uint16_t value)
{
    persistent_config_ram.main_menu_selected_tab = value;
}

uint16_t odroid_settings_MainMenuCursor_get()
{
    return persistent_config_ram.main_menu_cursor;
}
void odroid_settings_MainMenuCursor_set(uint16_t value)
{
    persistent_config_ram.main_menu_cursor = value;
}

void odroid_settings_MainMenuBrowseSubpath_set(const char *subpath)
{
    if (subpath && subpath[0]) {
        strncpy(persistent_config_ram.main_menu_browse_subpath,
                subpath,
                sizeof(persistent_config_ram.main_menu_browse_subpath) - 1);
        persistent_config_ram.main_menu_browse_subpath[sizeof(persistent_config_ram.main_menu_browse_subpath) - 1] = '\0';
    } else {
        persistent_config_ram.main_menu_browse_subpath[0] = '\0';
    }
}

bool odroid_settings_MainMenuBrowseSubpath_get(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0)
        return false;

    strncpy(buf, persistent_config_ram.main_menu_browse_subpath, buf_size - 1);
    buf[buf_size - 1] = '\0';
    return buf[0] != '\0';
}


int32_t odroid_settings_Palette_get()
{
    return persistent_config_ram.app[odroid_system_get_app()->id].palette;
}
void odroid_settings_Palette_set(int32_t value)
{
    persistent_config_ram.app[odroid_system_get_app()->id].palette = value;
}


int32_t odroid_settings_SpriteLimit_get()
{
    return persistent_config_ram.app[odroid_system_get_app()->id].sprite_limit;
}
void odroid_settings_SpriteLimit_set(int32_t value)
{
    persistent_config_ram.app[odroid_system_get_app()->id].sprite_limit = value;
}

// The 32X screen-tear guard borrows the per-app slot's sprite_limit field: the 32X
// core never reads sprite_limit, so on 32X the field is free storage, and old
// config files already carry 0 there -- exactly the guard's default (off). No
// struct growth, no version bump, existing /CONFIG loads unchanged.
int32_t odroid_settings_ScreenTearFix_get()
{
    return persistent_config_ram.app[odroid_system_get_app()->id].sprite_limit;
}
void odroid_settings_ScreenTearFix_set(int32_t value)
{
    persistent_config_ram.app[odroid_system_get_app()->id].sprite_limit = value;
}


ODROID_REGION odroid_settings_Region_get()
{
    return persistent_config_ram.app[odroid_system_get_app()->id].region;
}
void odroid_settings_Region_set(ODROID_REGION value)
{
    persistent_config_ram.app[odroid_system_get_app()->id].region = value;
}


int32_t odroid_settings_DisplayScaling_get()
{
    return persistent_config_ram.app[odroid_system_get_app()->id].disp_scaling;
}
void odroid_settings_DisplayScaling_set(int32_t value)
{
    persistent_config_ram.app[odroid_system_get_app()->id].disp_scaling = value;
}


int32_t odroid_settings_DisplayFilter_get()
{
    return persistent_config_ram.app[odroid_system_get_app()->id].disp_filter;
}
void odroid_settings_DisplayFilter_set(int32_t value)
{
    persistent_config_ram.app[odroid_system_get_app()->id].disp_filter = value;
}


int32_t odroid_settings_DisplayRotation_get()
{
  return odroid_settings_app_int32_get(Key_DispRotation, ODROID_DISPLAY_ROTATION_AUTO);
}
void odroid_settings_DisplayRotation_set(int32_t value)
{
  odroid_settings_app_int32_set(Key_DispRotation, value);
}


int32_t odroid_settings_DisplayOverscan_get()
{
    return persistent_config_ram.app[odroid_system_get_app()->id].disp_overscan;
}
void odroid_settings_DisplayOverscan_set(int32_t value)
{
    persistent_config_ram.app[odroid_system_get_app()->id].disp_overscan = value;
}


#if CHEAT_CODES == 1

static uint32_t read_active_cheats(char *game_path) {
    if (cheat_cache.is_cached && cheat_cache.game_path[0] && strcmp(cheat_cache.game_path, game_path) == 0) {
        return cheat_cache.active_cheat_codes;
    }

    uint32_t active_cheat_codes = 0;
    char *cheat_state_path = odroid_system_get_path(ODROID_PATH_CHEAT_STATE, game_path);
    if (odroid_sdcard_get_filesize(cheat_state_path) > 0) {
        FILE *cheat_state_file = fopen(cheat_state_path, "r");
        if (!cheat_state_file) {
            printf("Failed to open cheat state file %s\n", cheat_state_path);
            free(cheat_state_path);
            return 0;
        }
        fread(&active_cheat_codes, 1, sizeof(uint32_t), cheat_state_file);
        fclose(cheat_state_file);
    }
    free(cheat_state_path);

    // Update cache
    strncpy(cheat_cache.game_path, game_path, RG_PATH_MAX);
    cheat_cache.game_path[RG_PATH_MAX] = '\0';
    cheat_cache.active_cheat_codes = active_cheat_codes;
    cheat_cache.is_cached = true;

    return active_cheat_codes;
}

static void write_active_cheats(char *game_path, uint32_t active_cheat_codes) {
    char *cheat_state_path = odroid_system_get_path(ODROID_PATH_CHEAT_STATE, game_path);
    FILE *cheat_state_file = fopen(cheat_state_path, "w");
    if (!cheat_state_file) {
        printf("Failed to open cheat state file %s\n", cheat_state_path);
        free(cheat_state_path);
        return;
    }
    fwrite(&active_cheat_codes, 1, sizeof(uint32_t), cheat_state_file);
    fclose(cheat_state_file);
    free(cheat_state_path);

    // Update cache
    strncpy(cheat_cache.game_path, game_path, RG_PATH_MAX);
    cheat_cache.game_path[RG_PATH_MAX] = '\0';
    cheat_cache.active_cheat_codes = active_cheat_codes;
    cheat_cache.is_cached = true;
}

bool odroid_settings_ActiveGameGenieCodes_is_enabled(char *game_path, int code_index) {
    if (code_index > MAX_CHEAT_CODES) {
        return false;
    }

    return ((read_active_cheats(game_path) >> code_index) & 0x1) == 1;
}

bool odroid_settings_ActiveGameGenieCodes_set(char *game_path, int code_index, bool enable) {
    if (code_index > MAX_CHEAT_CODES) {
        return false;
    }

    uint32_t active_cheat_codes = read_active_cheats(game_path);
    if (enable) {
        active_cheat_codes |= (1 << code_index);
    } else {
        active_cheat_codes &= ~(1 << code_index);
    }
    write_active_cheats(game_path, active_cheat_codes);

    return true;
}
#else
bool odroid_settings_ActiveGameGenieCodes_is_enabled(char *game_path, int code_index) { return false; }
bool odroid_settings_ActiveGameGenieCodes_set(char *game_path, int code_index, bool enable) { return false; }
#endif


bool odroid_settings_DebugMenuDebugClockAlwaysOn_get()
{
    return persistent_config_ram.debug_clock_always_on;
}
void odroid_settings_DebugMenuDebugClockAlwaysOn_set(bool value)
{
    persistent_config_ram.debug_clock_always_on = value;
}

uint32_t odroid_settings_WelcomePrompt_get(void)
{
    return persistent_config_ram.welcome_prompt;
}

void odroid_settings_WelcomePrompt_set(uint32_t value)
{
    persistent_config_ram.welcome_prompt = value;
}

uint8_t odroid_settings_SortMode_get(void)
{
    uint8_t mode = persistent_config_ram.sort_mode;
    return (mode < ODROID_SORT_COUNT) ? mode : ODROID_SORT_NAME;
}

void odroid_settings_SortMode_set(uint8_t mode)
{
    persistent_config_ram.sort_mode = (mode < ODROID_SORT_COUNT) ? mode : ODROID_SORT_NAME;
}

uint8_t odroid_settings_CoverStyle_get(void)
{
    uint8_t style = persistent_config_ram.cover_style;
    return (style < ODROID_COVER_STYLE_COUNT) ? style : ODROID_COVER_STYLE_POSTER;
}

void odroid_settings_CoverStyle_set(uint8_t style)
{
    persistent_config_ram.cover_style =
        (style < ODROID_COVER_STYLE_COUNT) ? style : ODROID_COVER_STYLE_POSTER;
}
