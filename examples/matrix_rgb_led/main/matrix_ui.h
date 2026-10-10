/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "mosaico_matrix_led.h"

esp_err_t matrix_ui_init(void);

/* BSP_SUBBOARD_SLOT_COUNT selects the waiting screen. */
void matrix_ui_set_slot(bsp_subboard_slot_t slot);

/* Arrays contain 64 unscaled RGB values in LED index order. */
void matrix_ui_update(const uint8_t *red, const uint8_t *green, const uint8_t *blue, const char *effect);
