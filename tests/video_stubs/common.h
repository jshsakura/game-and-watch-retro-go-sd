/* Host-test stub. video_play.c only touches common_emu_state.skip_frames /
 * .pause_frames and reads the volume — the bitfield layout is copied from
 * Core/Inc/porting/common.h so the two stay bit-compatible if anything else
 * ever inspects the same struct in a shared test binary (nothing does today). */
#ifndef STUB_COMMON_H
#define STUB_COMMON_H
#include <stdint.h>

uint8_t common_emu_sound_get_volume(void);

typedef struct {
    uint32_t last_sync_time;
    uint32_t last_overlay_time;
    uint16_t skipped_frames;
    int16_t  frame_time_10us;
    uint8_t  skip_frames:2;
    uint8_t  pause_frames:1;
    uint8_t  pause_after_frames:3;
    uint8_t  startup_frames:2;
    uint8_t  overlay:4;
    uint8_t  clear_frames:2;
} common_emu_state_t;

extern common_emu_state_t common_emu_state;
#endif
