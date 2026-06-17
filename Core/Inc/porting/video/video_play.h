// Video playback loop: demux an AVI, decode + pace its MJPEG frames to the LCD
// (silent for now — audio comes next). Adapts to the file's size/rate and drops
// frames when it cannot keep up, so a too-heavy file still plays to the end
// rather than refusing. Truly undecodable input returns VID_UNPLAYABLE so the
// caller can show a message.
#pragma once

typedef enum {
    VID_OK = 0,        // reached the end of the movie
    VID_STOPPED,       // user pressed Back
    VID_UNPLAYABLE,    // not a usable AVI / no decodable video frames
} vid_result_t;

vid_result_t video_play(const char *path);
