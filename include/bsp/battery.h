/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t voltage_mv;
    int16_t current_ma;
    int16_t average_current_ma;
    int16_t average_power_mw;
    int16_t max_load_current_ma;
    int16_t standby_current_ma;
    float temperature_c;
    uint16_t status_flags;
    uint8_t state_of_charge;
    uint8_t state_of_health;
    uint16_t remaining_capacity_mah;
    uint16_t full_charge_capacity_mah;
    uint16_t cycle_count;
    uint16_t time_to_empty_min;
    uint16_t time_to_full_min;
} bsp_battery_status_t;

/**
 * @brief Attach the BQ27220 to the shared I2C bus and confirm it answers
 *
 * Applies BSP_BATTERY_CONFIG_DEFAULT() to the gauge RAM data memory. Matching
 * data is detected and left unchanged.
 */
esp_err_t bsp_battery_init(void);
esp_err_t bsp_battery_deinit(void);
esp_err_t bsp_battery_read(bsp_battery_status_t *status);
esp_err_t bsp_battery_read_basic(int16_t *voltage_mv, int16_t *current_ma, float *temperature_c);

#ifdef __cplusplus
}
#endif
