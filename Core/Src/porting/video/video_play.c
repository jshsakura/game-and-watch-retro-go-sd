// Video playback loop — see video_play.h.

#include "video_play.h"
#include "avi.h"
#include "video_decode.h"
#include "gw_lcd.h"
#include "main.h"               // HAL_GetTick / HAL_Delay / wdog_refresh
#include <odroid_system.h>      // gamepad input
#include <string.h>

vid_result_t video_play(const char *path)
{
    avi_t a;
    if (!avi_open(&a, path))
        return VID_UNPLAYABLE;              // not an AVI / no movi / corrupt header

    const int frame_ms = avi_frame_ms(&a);

    odroid_gamepad_state_t joy, prev;
    memset(&prev, 0, sizeof prev);

    uint32_t t0 = HAL_GetTick();            // playback clock origin
    uint32_t paused_ms = 0;                 // total time spent paused
    int  vframe = 0;                        // video frames seen (for scheduling)
    bool decoded_any = false;
    bool stopped = false;
    bool paused = false;

    long sz;
    avi_kind_t k;
    while ((k = avi_next(&a, &sz)) != AVI_END) {
        wdog_refresh();

        odroid_input_read_gamepad(&joy);
        #define PRESS(b) (joy.values[b] && !prev.values[b])
        if (PRESS(ODROID_INPUT_B)) { stopped = true; prev = joy; break; }
        if (PRESS(ODROID_INPUT_A)) paused = !paused;
        prev = joy;

        if (paused) {
            // Hold on the current frame; freeze the clock until unpaused.
            uint32_t pstart = HAL_GetTick();
            while (paused) {
                wdog_refresh();
                odroid_input_read_gamepad(&joy);
                if (PRESS(ODROID_INPUT_A)) paused = false;
                if (PRESS(ODROID_INPUT_B)) { stopped = true; paused = false; }
                prev = joy;
                HAL_Delay(20);
            }
            paused_ms += HAL_GetTick() - pstart;
            if (stopped) break;
        }

        if (k != AVI_VIDEO) continue;       // silent for now: skip audio chunks

        uint32_t elapsed = HAL_GetTick() - t0 - paused_ms;
        uint32_t due = (uint32_t)vframe * frame_ms;
        vframe++;

        // Behind by more than a frame -> drop (don't decode) to catch up.
        if (elapsed > due + (uint32_t)frame_ms)
            continue;

        if (video_decode_frame(a.f, sz, lcd_get_active_buffer(),
                               GW_LCD_WIDTH, GW_LCD_HEIGHT))
            decoded_any = true;
        // else: decode failed -> keep the previous frame on screen.

        // Pace: wait until this frame is due (only if we are early).
        while ((HAL_GetTick() - t0 - paused_ms) < due) {
            wdog_refresh();
            HAL_Delay(1);
        }
        lcd_swap();
    }

    avi_close(&a);
    if (!decoded_any) return VID_UNPLAYABLE;   // movi had no decodable frames
    return stopped ? VID_STOPPED : VID_OK;
}
