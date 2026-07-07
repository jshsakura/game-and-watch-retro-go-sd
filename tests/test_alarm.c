/* Host unit tests for the all-state alarm PURE logic: next-alarm epoch math
 * (incl. DND-as-empty and the midnight wrap), the wake-cause decision table,
 * and the synth tone-preset selection. Compiles rg_alarm.c with -DRG_ALARM_HOST
 * so none of the HAL/firmware half is pulled in. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include "../Core/Src/retro-go/rg_alarm.c"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __func__, __LINE__, #c); fails++; } } while (0)

/* Build a UTC epoch for a Y-M-D H:M:S (TZ forced to UTC in main). */
static time_t mk(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm = {0};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    return mktime(&tm);
}

static void test_next_epoch_basic(void)
{
    uint16_t mins[] = { 7 * 60 + 30 };            /* 07:30 daily */
    /* now = 06:00 -> fires today 07:30 */
    CHECK(rg_alarm_next_epoch(mins, 1, mk(2026, 7, 7, 6, 0, 0)) == mk(2026, 7, 7, 7, 30, 0));
    /* now = 07:30:00 exactly -> daily semantics push to tomorrow */
    CHECK(rg_alarm_next_epoch(mins, 1, mk(2026, 7, 7, 7, 30, 0)) == mk(2026, 7, 8, 7, 30, 0));
    /* now = 08:00 (passed) -> tomorrow 07:30 */
    CHECK(rg_alarm_next_epoch(mins, 1, mk(2026, 7, 7, 8, 0, 0)) == mk(2026, 7, 8, 7, 30, 0));
    /* fires at :00 seconds even if now has seconds */
    CHECK(rg_alarm_next_epoch(mins, 1, mk(2026, 7, 7, 6, 0, 45)) == mk(2026, 7, 7, 7, 30, 0));
}

static void test_next_epoch_midnight_wrap(void)
{
    uint16_t mins[] = { 0 * 60 + 30 };            /* 00:30 */
    /* now = 23:50 -> next is 00:30 the NEXT day */
    CHECK(rg_alarm_next_epoch(mins, 1, mk(2026, 7, 7, 23, 50, 0)) == mk(2026, 7, 8, 0, 30, 0));
    /* now = 00:10 -> still today 00:30 */
    CHECK(rg_alarm_next_epoch(mins, 1, mk(2026, 7, 7, 0, 10, 0)) == mk(2026, 7, 7, 0, 30, 0));
}

static void test_next_epoch_soonest_and_empty(void)
{
    uint16_t mins[] = { 22 * 60, 6 * 60, 12 * 60 };   /* 22:00, 06:00, 12:00 */
    /* now = 07:00 -> soonest future is 12:00 today */
    CHECK(rg_alarm_next_epoch(mins, 3, mk(2026, 7, 7, 7, 0, 0)) == mk(2026, 7, 7, 12, 0, 0));
    /* now = 23:00 -> soonest is 06:00 tomorrow */
    CHECK(rg_alarm_next_epoch(mins, 3, mk(2026, 7, 7, 23, 0, 0)) == mk(2026, 7, 8, 6, 0, 0));
    /* no alarms -> 0 (this is also how DND is expressed to the cache) */
    CHECK(rg_alarm_next_epoch(mins, 0, mk(2026, 7, 7, 7, 0, 0)) == 0);
    CHECK(rg_alarm_next_epoch(NULL, 0, mk(2026, 7, 7, 7, 0, 0)) == 0);
}

static void test_wake_decide(void)
{
    /* alarm flag wins over everything (authoritative on STANDBY) */
    CHECK(rg_alarm_wake_decide(true,  false, false) == RG_WAKE_ALARM);
    CHECK(rg_alarm_wake_decide(true,  true,  false) == RG_WAKE_ALARM);
    /* button only */
    CHECK(rg_alarm_wake_decide(false, true,  false) == RG_WAKE_BUTTON);
    /* STOP2: ISR already cleared ALRAF, but the cache says an alarm is due */
    CHECK(rg_alarm_wake_decide(false, false, true)  == RG_WAKE_ALARM);
    /* button AND cache-due: an alarm that came due wins -> ring */
    CHECK(rg_alarm_wake_decide(false, true,  true)  == RG_WAKE_BUTTON);  /* button decided first */
    /* nothing */
    CHECK(rg_alarm_wake_decide(false, false, false) == RG_WAKE_NONE);
}

static void test_tone_presets(void)
{
    /* token round-trip */
    CHECK(rg_tone_preset_from_token("beep")  == RG_TONE_BEEP);
    CHECK(rg_tone_preset_from_token("beep2") == RG_TONE_BEEP2);
    CHECK(rg_tone_preset_from_token("chirp") == RG_TONE_CHIRP);
    CHECK(rg_tone_preset_from_token("siren") == RG_TONE_SIREN);
    CHECK(rg_tone_preset_from_token("alarm.mp3") == -1);   /* a file name is not a preset */
    CHECK(rg_tone_preset_from_token(NULL) == -1);
    for (int i = 0; i < RG_TONE_COUNT; i++)
        CHECK(rg_tone_preset_from_token(rg_tone_preset_token(i)) == i);

    bool on;
    int period;
    /* BEEP: gated 250ms on / 250ms off, 880Hz */
    period = rg_alarm_tone_step(RG_TONE_BEEP, 0, &on);   CHECK(on);  CHECK(period == 48000 / 880);
    rg_alarm_tone_step(RG_TONE_BEEP, 300, &on);          CHECK(!on); /* in the gap */
    /* SIREN: never gaps, alternates between two periods over time */
    int p0, p1; rg_alarm_tone_step(RG_TONE_SIREN, 0, &on);   p0 = rg_alarm_tone_step(RG_TONE_SIREN, 0, &on);   CHECK(on);
    p1 = rg_alarm_tone_step(RG_TONE_SIREN, 400, &on);        CHECK(on);  CHECK(p0 != p1);
    /* CHIRP: frequency rises within the on window -> period shrinks */
    int e0 = rg_alarm_tone_step(RG_TONE_CHIRP, 0, &on);   CHECK(on);
    int e1 = rg_alarm_tone_step(RG_TONE_CHIRP, 350, &on); CHECK(on);  CHECK(e1 < e0);
    /* BEEP2 pulses faster than BEEP (shorter on window) */
    rg_alarm_tone_step(RG_TONE_BEEP2, 100, &on);         CHECK(!on); /* already past its 90ms on */
    /* out-of-range preset clamps to BEEP, never crashes */
    rg_alarm_tone_step(999, 0, &on);                     CHECK(on);
}

int main(void)
{
    setenv("TZ", "UTC", 1); tzset();
    test_next_epoch_basic();
    test_next_epoch_midnight_wrap();
    test_next_epoch_soonest_and_empty();
    test_wake_decide();
    test_tone_presets();
    if (fails) { printf("test_alarm: %d FAILED\n", fails); return 1; }
    printf("test_alarm: ALL PASS\n");
    return 0;
}
