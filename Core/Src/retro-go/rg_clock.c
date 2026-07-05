/* Full-screen Clock app — see rg_clock.h. Mode switcher over a live clock
 * face, a Pomodoro cycle, a countdown timer and a stopwatch. Every face is
 * drawn procedurally (a 7-segment vector font built from filled rects), so
 * there is no bundled art and nothing copyrighted to ship. Alarm and SD
 * theming (backgrounds / fonts) are layered on in later phases. */

#include <odroid_system.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "gw_lcd.h"
#include "rg_rtc.h"
#include "rg_i18n.h"
#include "gui.h"
#include "odroid_overlay.h"
#include "odroid_input.h"
#include "rg_clock.h"

/* ---- 7-segment vector digit ------------------------------------------- */

/* Segment bitmask per digit, bit0=a(top) .. bit6=g(middle), standard order. */
static const uint8_t SEG7[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

/* Draw one digit in a w x h cell at (x,y) with segment thickness t. */
static void draw_digit(int d, int x, int y, int w, int h, int t, uint16_t col)
{
    if (d < 0 || d > 9)
        return;
    uint8_t m = SEG7[d];
    int vlen = (h - 3 * t) / 2;              /* length of a vertical segment */
    int rx = x + w - t;                      /* right column x */
    int by = y + h - t;                      /* bottom row y */
    int my = y + t + vlen;                   /* middle row y */
    int ey = y + 2 * t + vlen;               /* lower verticals y */
    if (m & 0x01) odroid_overlay_draw_fill_rect(x + t, y,   w - 2 * t, t, col); /* a */
    if (m & 0x02) odroid_overlay_draw_fill_rect(rx,    y + t, t, vlen, col);    /* b */
    if (m & 0x04) odroid_overlay_draw_fill_rect(rx,    ey,   t, vlen, col);     /* c */
    if (m & 0x08) odroid_overlay_draw_fill_rect(x + t, by,   w - 2 * t, t, col);/* d */
    if (m & 0x10) odroid_overlay_draw_fill_rect(x,     ey,   t, vlen, col);     /* e */
    if (m & 0x20) odroid_overlay_draw_fill_rect(x,     y + t, t, vlen, col);    /* f */
    if (m & 0x40) odroid_overlay_draw_fill_rect(x + t, my,   w - 2 * t, t, col);/* g */
}

/* Draw "AB:CD" (four digits + colon) centred on x_center. Returns nothing. */
static void draw_time_4(int a, int b, int c, int e, int x_center, int y,
                        int w, int h, int t, bool colon_on, uint16_t col)
{
    int gap = t;                             /* space between digits */
    int colon_w = t + gap * 2;
    int total = 4 * w + 3 * gap + colon_w;
    int x = x_center - total / 2;

    draw_digit(a, x, y, w, h, t, col); x += w + gap;
    draw_digit(b, x, y, w, h, t, col); x += w + gap;
    int cx = x + gap;
    if (colon_on) {
        int r = t, cy = y + h / 3;
        odroid_overlay_draw_fill_rect(cx, cy,         r, r, col);
        odroid_overlay_draw_fill_rect(cx, y + 2 * h / 3, r, r, col);
    }
    x += colon_w;
    draw_digit(c, x, y, w, h, t, col); x += w + gap;
    draw_digit(e, x, y, w, h, t, col);
}

/* ---- app state -------------------------------------------------------- */

typedef enum { MODE_CLOCK = 0, MODE_POMODORO, MODE_TIMER, MODE_STOPWATCH, MODE_COUNT } clock_mode_t;

static const char *const MODE_NAME[MODE_COUNT] = { "CLOCK", "POMODORO", "TIMER", "STOPWATCH" };

/* Pomodoro / timer / stopwatch keep their state across mode switches within
 * one session (static), like a phone's clock app. */
typedef enum { RUN_STOPPED = 0, RUN_RUNNING, RUN_PAUSED } run_state_t;

typedef struct {
    run_state_t state;
    uint32_t    remaining_ms;   /* timer / pomodoro countdown */
    uint32_t    elapsed_ms;     /* stopwatch count-up */
    uint32_t    last_tick;      /* HAL_GetTick at last update */
} runner_t;

static runner_t s_timer  = { RUN_STOPPED, 5 * 60 * 1000, 0, 0 };   /* default 5:00 */
static runner_t s_watch  = { RUN_STOPPED, 0, 0, 0 };

/* Pomodoro */
static int  s_pomo_work_min  = 25;
static int  s_pomo_break_min = 5;
static bool s_pomo_on_break  = false;
static int  s_pomo_cycles    = 0;
static runner_t s_pomo = { RUN_STOPPED, 25 * 60 * 1000, 0, 0 };

/* Visual completion flash (until this tick). */
static uint32_t s_flash_until = 0;

static void tick_countdown(runner_t *r, uint32_t now)
{
    if (r->state != RUN_RUNNING)
        return;
    uint32_t dt = now - r->last_tick;
    r->last_tick = now;
    if (dt >= r->remaining_ms) {
        r->remaining_ms = 0;
        r->state = RUN_STOPPED;
    } else {
        r->remaining_ms -= dt;
    }
}

static void tick_countup(runner_t *r, uint32_t now)
{
    if (r->state != RUN_RUNNING)
        return;
    r->elapsed_ms += now - r->last_tick;
    r->last_tick = now;
}

static void runner_toggle(runner_t *r, uint32_t now)
{
    if (r->state == RUN_RUNNING) {
        r->state = RUN_PAUSED;
    } else {
        r->state = RUN_RUNNING;
        r->last_tick = now;
    }
}

/* ---- rendering -------------------------------------------------------- */

static const char *WD[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };

static void draw_header(clock_mode_t mode)
{
    char line[48];
    snprintf(line, sizeof line, "< %s >", MODE_NAME[mode]);
    odroid_overlay_draw_text(0, 6, GW_LCD_WIDTH, line, curr_colors->sel_c, 0x0000);
}

static void draw_hint(const char *hint)
{
    odroid_overlay_draw_text(0, GW_LCD_HEIGHT - 16, GW_LCD_WIDTH, hint,
                             curr_colors->dis_c, 0x0000);
}

static void render_clock(void)
{
    int hh = GW_GetCurrentHour(), mm = GW_GetCurrentMinute();
    bool colon = GW_GetCurrentSubSeconds() <= 127;
    draw_time_4(hh / 10, hh % 10, mm / 10, mm % 10,
                GW_LCD_WIDTH / 2, 70, 44, 92, 10, colon, curr_colors->main_c);

    char date[32];
    int wd = GW_GetCurrentWeekday();
    if (wd < 1 || wd > 7) wd = 1;
    snprintf(date, sizeof date, "20%02d-%02d-%02d  %s",
             GW_GetCurrentYear(), GW_GetCurrentMonth(), GW_GetCurrentDay(), WD[wd - 1]);
    odroid_overlay_draw_text(0, 180, GW_LCD_WIDTH, date, curr_colors->main_c, 0x0000);
    draw_hint("A: exit    L/R: mode");
}

static void render_mmss(uint32_t ms, uint16_t col, bool colon)
{
    uint32_t total = ms / 1000;
    int m = (total / 60) % 100, s = total % 60;
    draw_time_4(m / 10, m % 10, s / 10, s % 10,
                GW_LCD_WIDTH / 2, 74, 44, 92, 10, colon, col);
}

static void render_pomodoro(uint32_t now)
{
    tick_countdown(&s_pomo, now);
    if (s_pomo.state == RUN_STOPPED && s_pomo.remaining_ms == 0) {
        /* phase finished: flash, switch work<->break, reload, auto-continue */
        s_flash_until = now + 800;
        s_pomo_on_break = !s_pomo_on_break;
        if (!s_pomo_on_break) s_pomo_cycles++;
        s_pomo.remaining_ms = (s_pomo_on_break ? s_pomo_break_min : s_pomo_work_min) * 60u * 1000u;
        s_pomo.state = RUN_RUNNING;
        s_pomo.last_tick = now;
    }
    bool colon = (now / 500) & 1;
    render_mmss(s_pomo.remaining_ms, s_pomo_on_break ? curr_colors->dis_c : curr_colors->main_c,
                s_pomo.state != RUN_RUNNING ? true : colon);

    char st[48];
    snprintf(st, sizeof st, "%s   cycle %d", s_pomo_on_break ? "BREAK" : "WORK", s_pomo_cycles + 1);
    odroid_overlay_draw_text(0, 182, GW_LCD_WIDTH, st, curr_colors->sel_c, 0x0000);
    draw_hint(s_pomo.state == RUN_RUNNING ? "A: pause  SELECT: reset  B: exit"
                                          : "A: start  UP/DN: work min  B: exit");
}

static void render_timer(uint32_t now)
{
    tick_countdown(&s_timer, now);
    if (s_timer.state == RUN_STOPPED && s_timer.remaining_ms == 0 && s_flash_until < now)
        s_flash_until = now + 800;
    bool colon = (now / 500) & 1;
    render_mmss(s_timer.remaining_ms, curr_colors->main_c,
                s_timer.state != RUN_RUNNING ? true : colon);
    draw_hint(s_timer.state == RUN_RUNNING ? "A: pause  SELECT: reset  B: exit"
                                           : "A: start  UP/DN: +/-1 min  B: exit");
}

static void render_stopwatch(uint32_t now)
{
    tick_countup(&s_watch, now);
    render_mmss(s_watch.elapsed_ms, curr_colors->main_c, true);
    char cs[16];
    snprintf(cs, sizeof cs, ".%02u", (unsigned)((s_watch.elapsed_ms / 10) % 100));
    odroid_overlay_draw_text(0, 182, GW_LCD_WIDTH, cs, curr_colors->sel_c, 0x0000);
    draw_hint(s_watch.state == RUN_RUNNING ? "A: pause  SELECT: reset  B: exit"
                                           : "A: start  SELECT: reset  B: exit");
}

/* ---- input per mode --------------------------------------------------- */

static void input_pomodoro(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (k->values[ODROID_INPUT_A] && !p->values[ODROID_INPUT_A]) {
        if (s_pomo.state == RUN_STOPPED) {
            s_pomo_on_break = false;
            s_pomo.remaining_ms = s_pomo_work_min * 60u * 1000u;
        }
        runner_toggle(&s_pomo, now);
    }
    if (k->values[ODROID_INPUT_SELECT] && !p->values[ODROID_INPUT_SELECT]) {
        s_pomo.state = RUN_STOPPED; s_pomo_on_break = false; s_pomo_cycles = 0;
        s_pomo.remaining_ms = s_pomo_work_min * 60u * 1000u;
    }
    if (s_pomo.state != RUN_RUNNING) {
        if (k->values[ODROID_INPUT_UP] && !p->values[ODROID_INPUT_UP] && s_pomo_work_min < 90)
            s_pomo.remaining_ms = (++s_pomo_work_min) * 60u * 1000u;
        if (k->values[ODROID_INPUT_DOWN] && !p->values[ODROID_INPUT_DOWN] && s_pomo_work_min > 1)
            s_pomo.remaining_ms = (--s_pomo_work_min) * 60u * 1000u;
    }
}

static void input_timer(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (k->values[ODROID_INPUT_A] && !p->values[ODROID_INPUT_A])
        runner_toggle(&s_timer, now);
    if (k->values[ODROID_INPUT_SELECT] && !p->values[ODROID_INPUT_SELECT]) {
        s_timer.state = RUN_STOPPED;
        s_timer.remaining_ms = 5 * 60 * 1000;
    }
    if (s_timer.state != RUN_RUNNING) {
        if (k->values[ODROID_INPUT_UP] && !p->values[ODROID_INPUT_UP])
            s_timer.remaining_ms += 60 * 1000;
        if (k->values[ODROID_INPUT_DOWN] && !p->values[ODROID_INPUT_DOWN] && s_timer.remaining_ms >= 60 * 1000)
            s_timer.remaining_ms -= 60 * 1000;
    }
}

static void input_stopwatch(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (k->values[ODROID_INPUT_A] && !p->values[ODROID_INPUT_A])
        runner_toggle(&s_watch, now);
    if (k->values[ODROID_INPUT_SELECT] && !p->values[ODROID_INPUT_SELECT]) {
        s_watch.state = RUN_STOPPED; s_watch.elapsed_ms = 0;
    }
}

/* ---- main loop -------------------------------------------------------- */

void rg_clock_show(void)
{
    clock_mode_t mode = MODE_CLOCK;
    odroid_gamepad_state_t k, prev = {0};

    /* Swallow the button that opened the app so we don't act on its release. */
    odroid_input_read_gamepad(&prev);

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        uint32_t now = HAL_GetTick();

        /* CLOCK mode: A exits (it has no timer to start). Other modes: B exits. */
        bool exit_app = k.values[ODROID_INPUT_POWER];
        if (mode == MODE_CLOCK)
            exit_app |= (k.values[ODROID_INPUT_A] && !prev.values[ODROID_INPUT_A]);
        else
            exit_app |= (k.values[ODROID_INPUT_B] && !prev.values[ODROID_INPUT_B]);
        if (exit_app)
            break;

        /* L/R switches mode. */
        if (k.values[ODROID_INPUT_LEFT] && !prev.values[ODROID_INPUT_LEFT])
            mode = (mode == 0) ? MODE_COUNT - 1 : mode - 1;
        if (k.values[ODROID_INPUT_RIGHT] && !prev.values[ODROID_INPUT_RIGHT])
            mode = (mode + 1) % MODE_COUNT;

        switch (mode) {
        case MODE_POMODORO:  input_pomodoro(&k, &prev, now);  break;
        case MODE_TIMER:     input_timer(&k, &prev, now);     break;
        case MODE_STOPWATCH: input_stopwatch(&k, &prev, now); break;
        default: break;
        }

        /* Paint. */
        uint16_t *fb = lcd_get_active_buffer();
        bool flash = s_flash_until > now && ((now / 150) & 1);
        memset(fb, flash ? 0xFF : 0x00, GW_LCD_WIDTH * GW_LCD_HEIGHT * 2);

        draw_header(mode);
        switch (mode) {
        case MODE_CLOCK:     render_clock();       break;
        case MODE_POMODORO:  render_pomodoro(now); break;
        case MODE_TIMER:     render_timer(now);    break;
        case MODE_STOPWATCH: render_stopwatch(now);break;
        default: break;
        }

        lcd_swap();
        lcd_sleep_while_swap_pending();
        HAL_Delay(20);
        prev = k;
    }
}
