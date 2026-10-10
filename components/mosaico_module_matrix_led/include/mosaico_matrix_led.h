/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "bsp/subboard.h"
#include "led_strip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_MATRIX_LED_WIDTH  8
#define MOSAICO_MATRIX_LED_HEIGHT 8

/** Create a WS2812 matrix on an explicit slot. Caller enables board power and owns the slot GPIO. */
esp_err_t mosaico_matrix_led_new(bsp_subboard_slot_t slot, led_strip_handle_t *out_strip);

/** Set a buffered pixel: origin at the panel's LED0 corner, x across columns, y up. No slot rotation. */
esp_err_t mosaico_matrix_led_set_pixel(led_strip_handle_t strip, int x, int y, uint8_t r, uint8_t g, uint8_t b);

/* Use led_strip_refresh/clear/del for output and cleanup; serialize access to each handle. */

#ifdef __cplusplus
}
#endif
