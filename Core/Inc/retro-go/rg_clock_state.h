#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Shared types + persistent state between the resident clock driver
 * (rg_clock_ring.c: main loop, ring/alarm/mp3 dispatch, config I/O, runner
 * ticking) and the overlay clock UI (rg_clock.c: rendering, settings menu,
 * editors — linked into .overlay_clock, RAM_EMU).
 *
 * Every variable here is DEFINED (non-static) in rg_clock_ring.c so it lives
 * in resident DTCM/.bss, not RAM_EMU — it survives an MP3-alarm interlude
 * (which overwrites .overlay_clock wholesale) intact. The overlay side only
 * ever reads/writes it through these extern declarations. */

#if SD_CARD == 1
#define CLOCK_SD_MEDIA 1
#else
#define CLOCK_SD_MEDIA 0
#endif

#define MAX_ALARMS 8

typedef struct { uint8_t hour, min, enabled; } alarm_t;

typedef enum { MODE_CLOCK = 0, MODE_POMODORO, MODE_TIMER, MODE_STOPWATCH, MODE_COUNT } clock_mode_t;

typedef enum { RUN_STOPPED = 0, RUN_RUNNING, RUN_PAUSED } run_state_t;
typedef struct { run_state_t state; uint32_t remaining_ms, elapsed_ms, last_tick; } runner_t;

#define ANIM_COUNT 5
#define ANIM_SCENE 2
#define ANIM_GIF   3
#define ANIM_PHOTO 4

extern int      s_theme;
extern int      s_face_override;
extern bool     s_hour24;
extern bool     s_dnd;
extern int      s_anim;
extern int      s_scene;
extern int8_t   s_alarm_volume;
extern int8_t   s_beep_preset;
extern alarm_t  s_alarms[MAX_ALARMS];
extern int      s_alarm_count;
extern int      s_last_fired_min;

#if CLOCK_SD_MEDIA
extern char     s_bgfile[32];
extern char     s_alarmsnd[32];
extern bool     s_album_used;
extern int      s_photo_speed;
extern uint32_t s_photo_next;
extern uint32_t s_fade_start;
extern bool     s_fade_swapped;
#endif

extern runner_t s_timer;
extern runner_t s_watch;
extern int      s_pomo_work_min, s_pomo_break_min, s_pomo_cycles;
extern bool     s_pomo_on_break;
extern runner_t s_pomo;
extern uint32_t s_flash_until;

/* Resident functions the overlay calls into. */
void clock_config_save(void);
void tone_feed(uint32_t now, bool ringing);
int  next_alarm(int now_h, int now_m, int *idx);

/* Resident accessor replacing rg_emulators_shared_file_buffer()/_bytes() for
 * the clock's GIF/photo background decode arena — see rg_clock_ring.c for why
 * the old shared_files pointer is unsafe once the clock itself is an overlay. */
uint8_t *clock_overlay_arena(size_t *out_bytes);

/* Overlay functions the resident loop calls into. */
void clock_overlay_frame(clock_mode_t mode, bool ringing, uint32_t now, uint32_t last_input, bool force_dirty);
bool clock_settings_menu(void);
