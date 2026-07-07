#pragma once
#include <stdbool.h>

/* Optional MP3/WAV alarm sound for the Clock app.
 *
 * When /clock/alarm.mp3 exists, a ringing alarm plays it (looped) instead of the
 * synthesised beep. The MP3 decoder is far too big for the ~full internal flash,
 * so it rides the Music overlay: while the Clock app is foreground no emulator is
 * loaded, so the RAM_EMU region (and the shared_files decode arena that the GIF /
 * photo background borrows) is free. clock_alarm_mp3_start() stages a headerless
 * copy of the Music overlay (shipped as /cores/clockmp3.bin) into RAM_EMU and
 * drives its streaming decoder through the SAME ISR-fed ring the Music app uses
 * (music_fill() in gw_audio.c). WAV files are handled for free by that decoder.
 *
 * Every entry point degrades gracefully to "use the beep" (missing file, missing
 * core bin, unreadable / undecodable audio): the alarm is NEVER silent.
 *
 * ARENA CONFLICT: the overlay overwrites RAM_EMU, which the live GIF / photo
 * background borrows as its decode arena. The CALLER therefore frees that arena
 * (clock_gif_free / clock_album_close) BEFORE clock_alarm_mp3_start() and
 * restores it AFTER clock_alarm_mp3_stop(). clock_alarm_mp3_available() only
 * probes the file and touches nothing, so the caller can skip that dance (keep
 * the background live) whenever there is no alarm file. */

/* Point the alarm at a basename under /clock (from the settings picker); NULL or
 * "" restores the default /clock/alarm.mp3 so an old cfg keeps working. */
void clock_alarm_mp3_set_file(const char *name);

/* /clock/alarm.mp3 (or the picked file) present and openable — cheap probe. */
bool clock_alarm_mp3_available(void);

/* Stage the decoder overlay and open the alarm file. Returns true if MP3/WAV
 * audio will play (caller must have freed the RAM_EMU arena first); false means
 * fall back to the beep (nothing was left playing). */
bool clock_alarm_mp3_start(void);

/* Per-loop service while ringing: refill the decode-ahead ring, loop at end of
 * file, and apply the current system volume (0..255, the volume_tbl value). */
void clock_alarm_mp3_service(int volume);

/* Hand the SAI back and close the file. Safe to call when never started. */
void clock_alarm_mp3_stop(void);

/* True between a successful start() and stop(). */
bool clock_alarm_mp3_active(void);
