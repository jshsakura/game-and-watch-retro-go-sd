/* Minimal HAL stand-in so the REAL Core/Inc/gw_lcd.h compiles on the host.
 *
 * The point of the harness is that it runs the same code the device runs. If we
 * wrote our own gw_lcd.h instead, lcd_pen_set/lcd_pen_run -- the inline
 * functions every single pixel goes through -- would be a different program,
 * and the harness would be proving nothing about the firmware. So the header
 * stays, and only the handful of HAL types it names are faked.
 *
 * These are opaque on purpose: gw_lcd.h uses them in prototypes the harness
 * never calls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { int unused; } SPI_HandleTypeDef;
typedef struct { int unused; } LTDC_HandleTypeDef;
typedef struct { int unused; } RTC_HandleTypeDef;
typedef struct { int unused; } DMA2D_HandleTypeDef;

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;

#define __IO volatile
