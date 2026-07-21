/* Boot-loop rescue: see gw_boot_rescue.h for the design.
 *
 * The counter lives in RTC_BKP_DR28 as 0xB007'000N: the magic in the high
 * half makes garbage (first boot ever, backup domain wiped by a full drain)
 * read as zero instead of as a random failure count.
 */
#include "gw_boot_rescue.h"

#define RESCUE_MAGIC          0xB0070000u
#define RESCUE_MAGIC_MASK     0xFFFF0000u
#define RESCUE_COUNT_MASK     0x0000FFFFu
/* Two whole boots must fail before the third stops at the rescue screen. */
#define RESCUE_FAILED_BOOTS   2u
/* "Alive" = the shared input poll has run this long and this often. A boot
 * that hangs never polls; a boot that reaches the launcher or a game polls
 * every frame. */
#define RESCUE_ALIVE_MS       8000u
#define RESCUE_ALIVE_POLLS    300u
/* Unattended rescue screen: power off instead of draining the battery. */
#define RESCUE_SCREEN_TIMEOUT_MS 60000u

static uint32_t failed_boots = 0;
static uint32_t alive_polls = 0;
static bool boot_marked_ok = false;
static bool force_launcher = false;

#ifdef BOOT_RESCUE_HOST_TEST
#include "boot_rescue_stubs.h"
#else
#include <stdio.h>

#include "stm32h7xx_hal.h"
#include "main.h"
#include "gw_buttons.h"
#include "gw_lcd.h"
#include "gw_ofw.h"
#include "odroid_colors.h"
#include "odroid_display.h"
#include "odroid_overlay.h"

extern RTC_HandleTypeDef hrtc;

static uint32_t rescue_bkp_read(void)
{
  return HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR28);
}

static void rescue_bkp_write(uint32_t value)
{
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR28, value);
}

static uint32_t rescue_now_ms(void)
{
  return HAL_GetTick();
}
#endif

void boot_rescue_note_boot_start(void)
{
  uint32_t reg = rescue_bkp_read();
  failed_boots = ((reg & RESCUE_MAGIC_MASK) == RESCUE_MAGIC)
                     ? (reg & RESCUE_COUNT_MASK)
                     : 0;
  rescue_bkp_write(RESCUE_MAGIC | ((failed_boots + 1) & RESCUE_COUNT_MASK));
}

bool boot_rescue_screen_due(void)
{
  return failed_boots >= RESCUE_FAILED_BOOTS;
}

bool boot_rescue_force_launcher(void)
{
  return force_launcher;
}

void boot_rescue_mark_alive_tick(void)
{
  if (boot_marked_ok) {
    return;
  }
  alive_polls++;
  if (alive_polls >= RESCUE_ALIVE_POLLS && rescue_now_ms() >= RESCUE_ALIVE_MS) {
    rescue_bkp_write(RESCUE_MAGIC);
    boot_marked_ok = true;
  }
}

void boot_rescue_mark_clean_shutdown(void)
{
  rescue_bkp_write(RESCUE_MAGIC);
  boot_marked_ok = true;
}

#ifdef BOOT_RESCUE_HOST_TEST
/* The interactive screen and the standby entry are hardware through and
 * through; the host test exercises the counter logic above. */
uint32_t boot_rescue_test_failed_boots(void) { return failed_boots; }
void boot_rescue_test_reset_session(void)
{
  failed_boots = 0;
  alive_polls = 0;
  boot_marked_ok = false;
  force_launcher = false;
}
#else

void boot_rescue_power_off_now(void)
{
  lcd_backlight_off();
  boot_magic_set(BOOT_MAGIC_STANDBY);
  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_LOW);
  HAL_PWREx_ClearWakeupFlag(PWR_FLAG_WKUP1);
  HAL_PWR_EnterSTANDBYMode();
  while (1) {
    HAL_NVIC_SystemReset();
  }
}

void boot_rescue_screen_show(void)
{
  char line[44];
  int y = 8;
  /* "Boot the original firmware" is only real on dual-boot installs, where
   * bank 1 holds the patched OFW and clearing the DR0 "BOOT" flag makes the
   * patch boot it instead of us (same mechanism the launcher's own OFW menu
   * item uses, rg_main.c). */
  const bool have_ofw = get_ofw_is_present();

  lcd_sync();
  lcd_reset_active_buffer();
  odroid_display_set_backlight(ODROID_BACKLIGHT_LEVEL6);

  y += odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, "BOOT RESCUE", C_RED, C_BLACK) * 2;
  snprintf(line, sizeof(line), "Last %u boots did not finish.",
           (unsigned)failed_boots);
  y += odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, line, C_WHITE, C_BLACK) * 2;
  y += odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, "TIME : boot to menu, skip resume", C_WHITE, C_BLACK);
  y += odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, "A    : boot normally", C_WHITE, C_BLACK);
  if (have_ofw) {
    y += odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, "GAME : boot original firmware", C_WHITE, C_BLACK);
  }
  y += odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, "POWER: power off", C_WHITE, C_BLACK);
  y += odroid_overlay_draw_text(0, y, GW_LCD_WIDTH, "Powers off by itself in 60s.", C_GRAY, C_BLACK) * 2;

  uint32_t start = rescue_now_ms();

  /* The power button was likely just used to switch the unit on — wait for
   * everything to be released before treating a press as an answer. */
  while (buttons_get() != 0) {
    wdog_refresh();
    if (rescue_now_ms() - start > RESCUE_SCREEN_TIMEOUT_MS) {
      boot_rescue_power_off_now();
    }
  }

  while (1) {
    uint32_t buttons = buttons_get();
    wdog_refresh();
    if (buttons & B_POWER) {
      boot_rescue_power_off_now();
    }
    if (buttons & B_TIME) {
      force_launcher = true;
      return;
    }
    if (buttons & B_A) {
      return;
    }
    if (have_ofw && (buttons & B_GAME)) {
      /* Tell the patched OFW to stop booting Retro-Go, then reset into it. */
      HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, 0x00000000);
      boot_rescue_mark_clean_shutdown();
      HAL_NVIC_SystemReset();
    }
    if (rescue_now_ms() - start > RESCUE_SCREEN_TIMEOUT_MS) {
      boot_rescue_power_off_now();
    }
  }
}
#endif
