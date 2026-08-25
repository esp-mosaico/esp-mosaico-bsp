/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_motor_init(void);
esp_err_t bsp_motor_set(bool on);
esp_err_t bsp_motor_set_strength(uint8_t percent);

#ifdef __cplusplus
}
#endif
