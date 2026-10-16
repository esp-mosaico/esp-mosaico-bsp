/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create LVGL thermal viewer on the already-started Mosaico display. */
esp_err_t thermal_ui_start(void);

#ifdef __cplusplus
}
#endif
