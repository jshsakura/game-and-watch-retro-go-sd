// Remember where a clip was left off, so reopening it does not start over.
//
// The store is a small text file on the card, written ONLY when playback has
// already stopped -- the player must not touch the SD while it is decoding (a
// write in the middle of the frame loop is how the FAT gets corrupted), and by
// the time these are called the audio is muted and the demuxer is closed.
#pragma once

// Frame to resume `path` at, or 0 for "from the start". Returns 0 for an unknown
// clip, for a position too near the beginning to be worth restoring, and for one
// that was watched to the end.
int  video_resume_get(const char *path);

// Record where `path` was left. `total` is the clip's frame count: a position in
// the last few seconds means "finished", which ERASES the entry rather than
// storing an end-of-file position that would make the next open restart at the
// credits.
void video_resume_put(const char *path, int frame, int total);
