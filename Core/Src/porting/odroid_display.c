#include <assert.h>

#include "odroid_system.h"
#include "odroid_display.h"
#include "gw_lcd.h"

static const uint8_t backlightLevels[] = {128, 130, 133, 139, 149, 162, 178, 198, 222, 255};
static odroid_display_backlight_t backlightLevel = ODROID_BACKLIGHT_LEVEL6;
static odroid_display_rotation_t rotationMode = ODROID_DISPLAY_ROTATION_OFF;
static odroid_display_scaling_t scalingMode = ODROID_DISPLAY_SCALING_FULL;
static odroid_display_filter_t filterMode = ODROID_DISPLAY_FILTER_SHARP;

short odroid_display_queue_update(odroid_video_frame_t *frame, odroid_video_frame_t *previousFrame)
{
    return SCREEN_UPDATE_FULL;
}

void odroid_display_write_rect(short left, short top, short width, short height, short stride, const uint16_t* buffer)
{
    pixel_t *dest = lcd_get_active_buffer();

    for (short y = 0; y < height; y++) {
        if ((y + top) >= GW_LCD_WIDTH) 
            return;
        pixel_t *dest_row = &dest[(y + top) * GW_LCD_WIDTH + left];
        memcpy(dest_row, &buffer[y * stride], width * sizeof(pixel_t));
    }
}

// Same as odroid_display_write_rect but stride is assumed to be width (for backwards compat)
void odroid_display_write(short left, short top, short width, short height, const uint16_t* buffer)
{
    odroid_display_write_rect(left, top, width, height, width, buffer);
}

void odroid_display_force_refresh(void)
{
    // forceVideoRefresh = true;
}

odroid_display_backlight_t odroid_display_get_backlight()
{
    return backlightLevel;
}

void odroid_display_set_backlight(odroid_display_backlight_t level)
{
    assert(level >= ODROID_BACKLIGHT_LEVEL0 && level < ODROID_BACKLIGHT_LEVEL_COUNT);

    backlightLevel = level;
    lcd_backlight_set(backlightLevels[level]);
    odroid_settings_Backlight_set(level);
}

uint8_t odroid_display_get_backlight_raw() {
    return backlightLevels[backlightLevel];
}

/* Raw DAC value for an ARBITRARY level, independent of the current
 * (persisted) system backlight setting. Lets a caller — the Clock app's own
 * dedicated brightness row — preview/apply a level without touching
 * odroid_settings via odroid_display_set_backlight(). Clamped so a
 * corrupt/out-of-range level can't index off the table. */
uint8_t odroid_display_backlight_raw_for_level(int level)
{
    if (level < ODROID_BACKLIGHT_LEVEL0) level = ODROID_BACKLIGHT_LEVEL0;
    if (level >= ODROID_BACKLIGHT_LEVEL_COUNT) level = ODROID_BACKLIGHT_LEVEL_COUNT - 1;
    return backlightLevels[level];
}

odroid_display_scaling_t odroid_display_get_scaling_mode(void)
{
    return scalingMode;
}

void odroid_display_set_scaling_mode(odroid_display_scaling_t mode)
{
    odroid_settings_DisplayScaling_set(mode);
    scalingMode = mode;
}

odroid_display_filter_t odroid_display_get_filter_mode(void)
{
    return filterMode;
}

void odroid_display_set_filter_mode(odroid_display_filter_t mode)
{
    odroid_settings_DisplayFilter_set(mode);
    filterMode = mode;
}

void odroid_display_init()
{
    backlightLevel = odroid_settings_Backlight_get();
    lcd_backlight_set(backlightLevels[backlightLevel]);

    scalingMode = odroid_settings_DisplayScaling_get();
    filterMode = odroid_settings_DisplayFilter_get();
    rotationMode = odroid_settings_DisplayRotation_get();

    printf("LCD init done.\n");
}
