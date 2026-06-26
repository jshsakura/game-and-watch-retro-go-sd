/* nhdoom host port: shadow of NRF52840Doom/src/main.h.
 * Replaces the hardware-pin configuration header with the minimal set of
 * config macros the portable engine actually consumes, and pulls in the
 * dummy nrf.h + shadow graphics.h.  No nRF GPIO/clock machinery. */
#ifndef MAIN_H
#define MAIN_H
#include <stdbool.h>
#include <stdint.h>
#include "nrf.h"
#include "graphics.h"

/* MINEWDONGLE=1 compiles out i_video.c / d_main.c onboard-GPIO button blocks. */
#define MINEWDONGLE 0
#define DEBUG_OUT_PRINTF 0
#define CACHE_ALL_COLORMAP_TO_RAM 0
#define GAMMA_CORRECTION 1

/* keyboard model: I2C gamepad => key_t is 16-bit, matching the engine. */
#define I2C_KEYBOARD     1
#define PARALLEL_KEYBOARD 2
#define RADIO_KEYBOARD   3
#define I2C_GAMEPAD      4
#define KEYBOARD I2C_GAMEPAD

/* key_t already typedef'd by the force-included prelude (uint16_t). */

/* Drive a level immediately: D_DoomLoop() auto-warps via G_DeferedInitNew. */
#define DEBUG_SETUP    1
#define AUTOSTART_GAME 1
#define START_MAP      5
#define SHOW_FPS       false

/* onboard button pins referenced by i_video.c only when !MINEWDONGLE; provide
 * harmless values so the (compiled) block parses.  GPIO_PORT() resolves to a
 * dummy struct pointer whose ->IN reads as "not pressed" (all ones). */
#define P0 0
#define P1 1
#define PORT_NUM_BTN_A P1
#define PORT_NUM_BTN_B P1
#define PIN_NUM_BTN_A 2
#define PIN_NUM_BTN_B 10
typedef struct { volatile uint32_t IN; volatile uint32_t PIN_CNF[32]; } NH_GPIO_Type;
extern NH_GPIO_Type nh_gpio_p0, nh_gpio_p1;
#define NRF_P0 (&nh_gpio_p0)
#define NRF_P1 (&nh_gpio_p1)
#define PORT(t) NRF_P ## t
#define GPIO_PORT(port) PORT(port)

/* irq guards: no-ops on host (no DMA ISR). */
static inline void nh_enable_irq(void) {}
static inline void nh_disable_irq(void) {}

#endif
