/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_INVALID_RESPONSE 4
#define ESP_ERR_INVALID_STATE 5
#define I2C_ADDR_BIT_LEN_7 0
#define esp_err_to_name(e) "mock"
typedef struct {
    int dev_addr_length;
    unsigned device_address;
    unsigned scl_speed_hz;
} i2c_device_config_t;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t *, size_t, int);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *, size_t, uint8_t *, size_t, int);
