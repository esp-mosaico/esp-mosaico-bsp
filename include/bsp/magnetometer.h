/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "bmm150.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_MAGNETOMETER_0 = 0,
    BSP_MAGNETOMETER_1,
    BSP_MAGNETOMETER_NUM,
} bsp_magnetometer_t;

esp_err_t bsp_magnetometer_init(bsp_magnetometer_t id);
esp_err_t bsp_magnetometer_init_all(void);
esp_err_t bsp_magnetometer_deinit(bsp_magnetometer_t id);
esp_err_t bsp_magnetometer_read(bsp_magnetometer_t id, struct bmm150_mag_data *data);
struct bmm150_dev *bsp_magnetometer_get_handle(bsp_magnetometer_t id);

#ifdef __cplusplus
}
#endif
