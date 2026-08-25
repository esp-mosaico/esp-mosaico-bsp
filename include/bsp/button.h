/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "button_gpio.h"
#include "iot_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_BUTTON_AI = 0,
    BSP_BUTTON_NUM,
} bsp_button_t;

esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size);

#ifdef __cplusplus
}
#endif
