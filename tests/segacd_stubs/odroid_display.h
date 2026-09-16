#ifndef SEGACD_STUBS_DISPLAY_H
#define SEGACD_STUBS_DISPLAY_H
typedef enum {
    ODROID_BACKLIGHT_LEVEL0,
    ODROID_BACKLIGHT_LEVEL1,
    ODROID_BACKLIGHT_LEVEL2,
    ODROID_BACKLIGHT_LEVEL3,
    ODROID_BACKLIGHT_LEVEL4,
    ODROID_BACKLIGHT_LEVEL5,
    ODROID_BACKLIGHT_LEVEL6,
} odroid_display_backlight_t;
void odroid_display_set_backlight(odroid_display_backlight_t level);
#endif
