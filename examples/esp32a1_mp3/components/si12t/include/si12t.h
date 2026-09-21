/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SI12T_I2C_ADDRESS       0x78U
#define SI12T_I2C_SPEED_HZ      100000U
#define SI12T_CHANNEL_COUNT     12U

typedef struct si12t_t *si12t_handle_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    uint8_t address;
    uint32_t clock_speed_hz;
    uint32_t timeout_ms;
} si12t_config_t;

#define SI12T_CONFIG_DEFAULT(bus_handle) { \
    .bus = (bus_handle),                   \
    .address = SI12T_I2C_ADDRESS,          \
    .clock_speed_hz = SI12T_I2C_SPEED_HZ,  \
    .timeout_ms = 50,                      \
}

esp_err_t si12t_create(const si12t_config_t *config, si12t_handle_t *out_handle);
/**
 * @brief Set selected channels to the most sensitive hardware setting.
 *
 * Bit 0 selects channel 1 and bit 11 selects channel 12.
 */
esp_err_t si12t_set_max_sensitivity(si12t_handle_t handle, uint16_t channel_mask);
/** Set selected SEN nibbles: bit 3 = ChHL, bits 2:0 = ChM (0..7).
 * Increasing ChM raises the detection thresholds and lowers sensitivity.
 */
esp_err_t si12t_set_sensitivity(si12t_handle_t handle, uint16_t channel_mask, uint8_t value);
/** Read all nonzero output levels (low/medium/high), matching CFIG.ILC=01. */
esp_err_t si12t_read_pressed(si12t_handle_t handle, uint16_t *pressed_mask);
esp_err_t si12t_delete(si12t_handle_t handle);

#ifdef __cplusplus
}
#endif
