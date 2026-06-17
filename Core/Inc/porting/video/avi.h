// Streaming AVI (RIFF) demuxer for the Video app.
//
// Walks the 'movi' list and hands back each chunk as a VIDEO frame (a baseline
// JPEG, FFD8…) or an AUDIO chunk (MP3, FFFB…), streamed straight from the file —
// the whole movie is never resident in RAM. Frames feed tjpgd and audio feeds
// minimp3, the very decoders the Music app already ships, so the Video app adds
// no new codecs. Pure logic over FILE*, so it is host-unit-testable.
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum { AVI_END = 0, AVI_VIDEO = 1, AVI_AUDIO = 2 } avi_kind_t;

#define AVI_CKPT_N 128       // sparse seek checkpoints (offset index, ~512 bytes)

typedef struct {
    FILE *f;
    long  file_end;          // total file size (bounds the header walk)
    long  movi_start;        // first chunk offset of the movi list (for seeking)
    long  movi_pos;          // read cursor within the movi list
    long  movi_end;          // end offset of the movi list
    int   width, height;     // video frame size (from the AVI main header)
    int   usec_per_frame;    // microseconds per frame (from the AVI main header)
    int   total_frames;      // total video frames (from the header; 0 if unknown)
    int   cur_frame;         // video frames consumed so far (advanced by avi_next)
    long  ckpt[AVI_CKPT_N];  // movi_pos recorded at frame k*ckpt_step (0 = not seen yet)
    int   ckpt_step;         // frames between checkpoints
} avi_t;

// Open `path`, parse the AVI main header (frame size + rate) and locate the
// 'movi' stream. Returns false (and leaves nothing open) if it is not a usable
// AVI. On success the cursor sits at the first movi chunk. `rabuf`/`rasize` is an
// optional read-ahead buffer (stdio fully-buffered) for large block reads; pass
// NULL/0 for default buffering.
bool avi_open(avi_t *a, const char *path, void *rabuf, unsigned long rasize);

// Advance to the next video/audio chunk. On AVI_VIDEO / AVI_AUDIO, sets *size to
// the chunk payload length and positions the file at the payload's first byte
// (read *size bytes — e.g. stream them into a decoder). Returns AVI_END at the
// end of the movi list. JUNK / index / empty / unknown chunks are skipped.
avi_kind_t avi_next(avi_t *a, long *size);

// Rewind the demuxer to the movi start and skip forward to the `frame`-th video
// frame (clamped to [0, total)). Cheap because MJPEG frames are independent.
// After this the next avi_next() returns that frame.
void avi_seek_frame(avi_t *a, int frame);

void avi_close(avi_t *a);

// Milliseconds per frame from the header (defaults to ~24fps if unspecified).
static inline int avi_frame_ms(const avi_t *a)
{
    int us = a->usec_per_frame > 0 ? a->usec_per_frame : 41667;
    return (us + 500) / 1000;
}
