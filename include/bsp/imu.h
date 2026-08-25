/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "bmi270.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t accel_odr;
    uint8_t accel_range;
    uint8_t gyro_odr;
    uint8_t gyro_range;
} bsp_imu_config_t;

#define BSP_IMU_CONFIG_DEFAULT() {      \
    .accel_odr = BMI2_ACC_ODR_100HZ,    \
    .accel_range = BMI2_ACC_RANGE_4G,   \
    .gyro_odr = BMI2_GYR_ODR_100HZ,     \
    .gyro_range = BMI2_GYR_RANGE_2000,  \
}

esp_err_t bsp_imu_init(void);
esp_err_t bsp_imu_start(const bsp_imu_config_t *config);
esp_err_t bsp_imu_stop(void);
esp_err_t bsp_imu_deinit(void);
esp_err_t bsp_imu_get_accel(float *x_g, float *y_g, float *z_g);
esp_err_t bsp_imu_get_gyro(float *x_dps, float *y_dps, float *z_dps);
struct bmi2_dev *bsp_imu_get_handle(void);

#ifdef __cplusplus
}
#endif
