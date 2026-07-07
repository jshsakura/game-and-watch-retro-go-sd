/* MP3/WAV alarm sound — see rg_clock_alarm_mp3.h.
 *
 * The whole trick: reuse the Music app's streaming decoder + the resident
 * ISR-fed ring (music_fill in gw_audio.c) without becoming a separate app.
 * audio_open/pump/seek/eof/close live in the Music overlay (.overlay_music);
 * the linker resolves them to their RAM_EMU VMA, so they are only callable once
 * clock_alarm_mp3_start() has copied the overlay bytes into RAM_EMU — exactly
 * how rg_emulators.c calls app_main_music() after staging the same overlay. */

#include "rg_clock_alarm_mp3.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "main.h"            /* wdog_refresh, SCB_CleanDCache_by_Addr (CMSIS) */
#include "gw_audio.h"        /* music_audio_enable/set, audio_start/stop_playing */
#include "gw_linker.h"       /* __RAM_EMU_START__, _OVERLAY_MUSIC_* */
#include "odroid_overlay.h"  /* odroid_overlay_cache_file_in_ram */

#ifndef CLOCK_ALARM_MP3_PATH
#define CLOCK_ALARM_MP3_PATH "/clock/alarm.mp3"
#endif
/* Headerless copy of .overlay_music shipped in /cores by the build (loaded with
 * the raw cache_file_in_ram path, like the homebrew Music.bin). */
#ifndef CLOCK_ALARM_CORE_PATH
#define CLOCK_ALARM_CORE_PATH "/cores/clockmp3.bin"
#endif

/* Mirror AUDIO_PUMP_TARGET (music_audio.h): keep the 8192-sample decode-ahead
 * ring nearly full so the SAI ISR never underruns between service passes. */
#define CLOCK_MP3_PUMP_TARGET (8192 - 1152)

/* Streaming decoder entry points — defined in the Music overlay. Declared here
 * (rather than pulling in music_audio.h + its include path) to keep this small
 * resident TU decoupled from the overlay build. */
extern bool audio_open(const char *path);
extern void audio_pump(int target);
extern void audio_seek(float frac);
extern bool audio_eof(void);
extern int  audio_ring_count(void);
extern void audio_close(void);

static bool s_active;

/* Chosen alarm sound basename (settings picker). Only a POINTER into the caller's
 * persistent buffer (rg_clock's s_alarmsnd) is kept — not a copy — so the tight
 * DTCM gains no resident string buffer; the full path is built on the stack per
 * use. NULL/"" = the default /clock/alarm.mp3 (back-compat with an old cfg). */
static const char *s_mp3_name;

void clock_alarm_mp3_set_file(const char *name)
{
    s_mp3_name = (name && name[0]) ? name : NULL;
}

/* Resolve the active alarm-sound path into `out` (or the static default). */
static const char *mp3_resolve_path(char *out, size_t n)
{
    if (!s_mp3_name) return CLOCK_ALARM_MP3_PATH;
    snprintf(out, n, "/clock/%s", s_mp3_name);
    return out;
}

bool clock_alarm_mp3_active(void) { return s_active; }

bool clock_alarm_mp3_available(void)
{
    char pbuf[64];
    FILE *f = fopen(mp3_resolve_path(pbuf, sizeof pbuf), "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

bool clock_alarm_mp3_start(void)
{
    s_active = false;

    /* Stage the decoder overlay into RAM_EMU. The caller has already freed the
     * shared_files arena (GIF/photo), so this cannot clobber a live decode. */
    wdog_refresh();
    if (odroid_overlay_cache_file_in_ram(CLOCK_ALARM_CORE_PATH,
                                         (uint8_t *)&__RAM_EMU_START__) == 0)
        return false;                       /* core bin missing/short -> beep */
    memset(&_OVERLAY_MUSIC_BSS_START, 0, (size_t)&_OVERLAY_MUSIC_BSS_SIZE);
    SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__,
                            (size_t)&_OVERLAY_MUSIC_SIZE);
    wdog_refresh();

    char pbuf[64];
    if (!audio_open(mp3_resolve_path(pbuf, sizeof pbuf)))
        return false;                       /* unreadable -> beep */

    /* Prime the ring; if nothing decodes (corrupt / empty file) fall back. */
    audio_pump(CLOCK_MP3_PUMP_TARGET);
    if (audio_eof() && audio_ring_count() == 0) {
        audio_close();
        return false;                       /* undecodable -> beep */
    }

    audio_start_playing(AUDIO_BUFFER_LENGTH);   /* (re)start the SAI DMA */
    music_audio_enable(1);                       /* ISR feeds from the decoder ring */
    s_active = true;
    return true;
}

void clock_alarm_mp3_service(int volume)
{
    if (!s_active)
        return;
    if (audio_eof() && audio_ring_count() == 0)
        audio_seek(0.0f);                   /* loop: rewind to the start (also pumps) */
    else if (!audio_eof())
        audio_pump(CLOCK_MP3_PUMP_TARGET);
    music_audio_set(volume, 1);             /* loudness tracks the system volume live */
}

void clock_alarm_mp3_stop(void)
{
    if (!s_active)
        return;
    music_audio_enable(0);                  /* ISR returns to core silence */
    audio_stop_playing();                   /* stop the SAI DMA */
    audio_close();
    s_active = false;
}
