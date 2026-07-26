#pragma once

typedef enum {
    APPID_LAUNCHER = 0,
    APPID_GB       = 1,
    APPID_NES      = 2,
    APPID_SMS      = 3,
    APPID_PCE      = 4,
    APPID_GW       = 5,
    APPID_MSX      = 6,
    APPID_WSV      = 7,
    APPID_MD       = 8,
    APPID_A7800    = 9,
    APPID_AMSTRAD  = 10,
    APPID_ZELDA3   = 11,
    APPID_SMW      = 12,
    APPID_VIDEOPAC = 13,
    APPID_HOMEBREW = 14,
    APPID_TAMA     = 15,
    APPID_A2600    = 16,
    APPID_PKMINI   = 17,
    APPID_WSWAN    = 18,
    APPID_PICO8    = 19,
    APPID_NGP      = 20,
    APPID_LYNX     = 21,
    APPID_VB       = 22,
    APPID_SM       = 23,   /* Super Metroid (SD builds only) */
    APPID_GBA      = 24,   /* Game Boy Advance (gpsp) */
    APPID_SNES     = 25,   /* generic SNES core (LakeSnes, SD builds only) */
    APPID_32X      = 26,   /* RETIRED 0727 -- Sega 32X core removed from the
                            * firmware (docs/32X_CLOSED.md). The SLOT STAYS:
                            * this enum sizes persistent_config_t's app[]
                            * array, so deleting an entry shrinks /CONFIG,
                            * fails its magic check and silently resets every
                            * user's language, coverflow, backlight and
                            * volume. Reuse it for a future core instead. */
    /* CPS-1 and Sega CD were removed here; both grew persistent_config_t and
     * their removal shrinks it, so /CONFIG stops matching and every user's
     * settings reset to defaults (CLAUDE.md). Accepted deliberately for this
     * release. Do not re-add a slot without bumping the config version. */

    APPID_COUNT,
} appid_t;

