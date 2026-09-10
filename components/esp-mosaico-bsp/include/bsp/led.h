/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the v1.0 status LED. Returns ESP_ERR_NOT_SUPPORTED on v1.2. */
esp_err_t bsp_led_init(void);
esp_err_t bsp_led_set(bool on);

#ifdef __cplusplus
}
#endif
