#pragma once
#include <stdint.h>

typedef struct
{
    uint16_t width;
    uint16_t height;
    char logo[];
} retro_logo_image;

/* Full-colour console icon for the launcher header, stored 4bpp + a 16-entry
 * RGB565 palette to keep internal flash small. Palette index 0 is transparent
 * (skipped at blit time). data is flat 4bpp, 2 px/byte, even px = high nibble. */
typedef struct
{
    uint16_t width;
    uint16_t height;
    const uint16_t *pal;
    const uint8_t  *data;
} color_icon_t;

/* Returns the colour icon for a RG_LOGO_PAD_* index, or NULL if none (caller
 * then falls back to the 1-bit pad_* logo). */
const color_icon_t *color_icon_for_logo(int16_t logo_idx);

enum {
    RG_LOGO_EMPTY = -1,
    RG_LOGO_RGO = 0,
    RG_LOGO_RGW,
//    RG_LOGO_FLASH,
    RG_LOGO_GNW,
    // Headers
    RG_LOGO_HEADER_SG1000,
    RG_LOGO_HEADER_COL,
    RG_LOGO_HEADER_GB,
    RG_LOGO_HEADER_GBC,
    RG_LOGO_HEADER_GG,
    RG_LOGO_HEADER_NES,
    RG_LOGO_HEADER_PCE,
    RG_LOGO_HEADER_SMS,
    RG_LOGO_HEADER_GW,
    RG_LOGO_HEADER_MSX,
    RG_LOGO_HEADER_GEN,
    RG_LOGO_HEADER_WSV,
    RG_LOGO_HEADER_A2600,
    RG_LOGO_HEADER_A7800,
    RG_LOGO_HEADER_AMSTRAD,
    RG_LOGO_HEADER_HOMEBREW,
    RG_LOGO_HEADER_TAMA,
    RG_LOGO_HEADER_PKMINI,
    // Pads
    RG_LOGO_PAD_SG1000,
    RG_LOGO_PAD_COL,
    RG_LOGO_PAD_GB,
    RG_LOGO_PAD_GG,
    RG_LOGO_PAD_NES,
    RG_LOGO_PAD_PCE,
    RG_LOGO_PAD_SMS,
    RG_LOGO_PAD_GW,
    RG_LOGO_PAD_MSX,
    RG_LOGO_PAD_GEN,
    RG_LOGO_PAD_WSV,
    RG_LOGO_PAD_A2600,
    RG_LOGO_PAD_A7800,
    RG_LOGO_PAD_AMSTRAD,
    RG_LOGO_PAD_SNES,
    RG_LOGO_PAD_TAMA,
    RG_LOGO_PAD_PKMINI,
    // Logos
    RG_LOGO_COLECO,
    RG_LOGO_NINTENDO,
    RG_LOGO_SEGA,
    RG_LOGO_PCE,
    RG_LOGO_MICROSOFT,
    RG_LOGO_WATARA,
    RG_LOGO_ATARI,
    RG_LOGO_AMSTRAD,
    RG_LOGO_TAMA,
    // PICO-8 (appended last to not shift any existing enum values)
    RG_LOGO_HEADER_PICO8,
    // NGP / WonderSwan (appended last to not shift existing enum values)
    RG_LOGO_HEADER_NGP,
    RG_LOGO_PAD_NGP,
    RG_LOGO_HEADER_WSWAN,
    RG_LOGO_PAD_WSWAN,
    // Homebrew beer-stein tab icon (appended last to not shift existing enum values)
    RG_LOGO_PAD_HOMEBREW,
    // Atari Lynx name header (appended last; matches header_lynx at end of rg_logos.c)
    RG_LOGO_HEADER_LYNX,
    // PC Engine CD name header (matches header_pcecd, appended after header_lynx)
    RG_LOGO_HEADER_PCECD,
    // Odyssey2 / ZX Spectrum / C64 name headers (match header_videopac/zx/c64,
    // appended last in rg_logos.c to keep /bios/logo.bin index alignment)
    RG_LOGO_HEADER_VIDEOPAC,
    RG_LOGO_HEADER_ZX,
    RG_LOGO_HEADER_C64,
    // Favorites virtual tab name header (matches header_favorites, appended last)
    RG_LOGO_HEADER_FAVORITES,
    // Tiger Game.com name header (matches header_gamecom — the last LOGO_DATA
    // struct in rg_logos.c, so it's a NEW last /bios/logo.bin entry and no
    // existing index shifts).
    RG_LOGO_HEADER_GAMECOM,
    // Colour-only console icons (color_icon_for_logo); no logo.bin entry, so
    // rg_get_logo() returns NULL for them (bounds-checked) — used only as the
    // header-right colour icon, never the 1-bit navbar logo.
    RG_LOGO_PAD_PICO8,
    RG_LOGO_PAD_GBC,
    RG_LOGO_PAD_LYNX,
    RG_LOGO_PAD_PCECD,
    // Odyssey2 / ZX Spectrum / C64 colour tab icons (color_icon_for_logo only,
    // no logo.bin entry -> rg_get_logo() returns NULL, bounds-checked)
    RG_LOGO_PAD_VIDEOPAC,
    RG_LOGO_PAD_ZX,
    RG_LOGO_PAD_C64,
    // Favorites virtual tab colour icon (gold star; color_icon_for_logo only)
    RG_LOGO_PAD_FAVORITES,
    // Tiger Game.com colour tab icon (cicon_gamecom; color_icon_for_logo only)
    RG_LOGO_PAD_GAMECOM,
    // Virtual Boy colour tab icon (cicon_vb; color_icon_for_logo only, no logo.bin
    // entry). Prepared ahead of the VB core so the asset is ready to wire up.
    RG_LOGO_PAD_VB,
};

void odroid_overlay_draw_logo(uint16_t x_pos, uint16_t y_pos, int16_t logo_idx, uint16_t color);
void rg_reset_logo_buffers();
retro_logo_image *rg_get_logo(int16_t logo_index);

extern const retro_logo_image logo_rgo;
//extern const retro_logo_image logo_flash;
extern const retro_logo_image logo_rgw;
extern const retro_logo_image logo_gnw;

extern const retro_logo_image header_sg1000;
extern const retro_logo_image header_col;
extern const retro_logo_image header_gb;
extern const retro_logo_image header_gbc;
extern const retro_logo_image header_gg;
extern const retro_logo_image header_nes;
extern const retro_logo_image header_pce;
extern const retro_logo_image header_sms;
extern const retro_logo_image header_gw;
extern const retro_logo_image header_msx;
extern const retro_logo_image header_wsv;
extern const retro_logo_image header_gen;
extern const retro_logo_image header_a2600;
extern const retro_logo_image header_a7800;
extern const retro_logo_image header_amstrad;
extern const retro_logo_image header_zelda3;
extern const retro_logo_image header_smw;
extern const retro_logo_image header_homebrew;
extern const retro_logo_image header_tama;
extern const retro_logo_image header_pkmini;
extern const retro_logo_image header_pico8;

extern const retro_logo_image pad_sg1000;
extern const retro_logo_image pad_col;
extern const retro_logo_image pad_gb;
extern const retro_logo_image pad_gg;
extern const retro_logo_image pad_nes;
extern const retro_logo_image pad_pce;
extern const retro_logo_image pad_sms;
extern const retro_logo_image pad_gw;
extern const retro_logo_image pad_msx;
extern const retro_logo_image pad_wsv;
extern const retro_logo_image pad_gen;
extern const retro_logo_image pad_a2600;
extern const retro_logo_image pad_a7800;
extern const retro_logo_image pad_amstrad;
extern const retro_logo_image pad_snes;
extern const retro_logo_image pad_tama;
extern const retro_logo_image pad_pkmini;

extern const retro_logo_image logo_coleco;
extern const retro_logo_image logo_nintendo;
extern const retro_logo_image logo_sega;
extern const retro_logo_image logo_pce;
extern const retro_logo_image logo_microsoft;
extern const retro_logo_image logo_watara;
extern const retro_logo_image logo_atari;
extern const retro_logo_image logo_amstrad;
extern const retro_logo_image logo_tama;
extern const retro_logo_image header_ngp;
extern const retro_logo_image pad_ngp;
extern const retro_logo_image header_wswan;
extern const retro_logo_image pad_wswan;
extern const retro_logo_image pad_homebrew;
extern const retro_logo_image header_lynx;
extern const retro_logo_image header_pcecd;
extern const retro_logo_image header_videopac;
extern const retro_logo_image header_zx;
extern const retro_logo_image header_c64;
extern const retro_logo_image header_favorites;
extern const retro_logo_image header_gamecom;


extern const unsigned char IMG_SPEAKER[];
extern const unsigned char IMG_SUN[];
extern const unsigned char IMG_FOLDER[];
extern const unsigned char IMG_DISKETTE[];
extern const unsigned char IMG_0_5X[];
extern const unsigned char IMG_0_75X[]; 
extern const unsigned char IMG_1X[];
extern const unsigned char IMG_1_25X[];
extern const unsigned char IMG_1_5X[];
extern const unsigned char IMG_2X[];
extern const unsigned char IMG_3X[];
extern const unsigned char IMG_SC[];
extern const unsigned char IMG_BUTTON_A[];
extern const unsigned char IMG_BUTTON_A_P[];
extern const unsigned char IMG_BUTTON_B[];
extern const unsigned char IMG_BUTTON_B_P[];

extern const unsigned char img_clock_00[];
extern const unsigned char img_clock_01[];
extern const unsigned char img_clock_02[];
extern const unsigned char img_clock_03[];
extern const unsigned char img_clock_04[];
extern const unsigned char img_clock_05[];
extern const unsigned char img_clock_06[];
extern const unsigned char img_clock_07[];
extern const unsigned char img_clock_08[];
extern const unsigned char img_clock_09[];

extern const unsigned char IMG_BORDER_ZELDA3[];
extern const unsigned char IMG_BORDER_LEFT_SMW[];
extern const unsigned char IMG_BORDER_RIGHT_SMW[];

