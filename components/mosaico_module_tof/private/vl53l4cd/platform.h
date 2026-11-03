/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_dev_handle_t i2c;
    esp_err_t error;
} mosaico_l4cd_platform_t;

typedef mosaico_l4cd_platform_t *Dev_t;

uint8_t VL53L4CD_RdByte(Dev_t dev, uint16_t reg, uint8_t *value);
uint8_t VL53L4CD_RdWord(Dev_t dev, uint16_t reg, uint16_t *value);
uint8_t VL53L4CD_RdDWord(Dev_t dev, uint16_t reg, uint32_t *value);
uint8_t VL53L4CD_WrByte(Dev_t dev, uint16_t reg, uint8_t value);
uint8_t VL53L4CD_WrWord(Dev_t dev, uint16_t reg, uint16_t value);
uint8_t VL53L4CD_WrDWord(Dev_t dev, uint16_t reg, uint32_t value);
uint8_t VL53L4CD_WaitMs(Dev_t dev, uint32_t ms);
