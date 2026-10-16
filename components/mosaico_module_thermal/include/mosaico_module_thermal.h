/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "bsp/subboard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_THERMAL_WIDTH 32
#define MOSAICO_THERMAL_HEIGHT 24
#define MOSAICO_THERMAL_PIXEL_COUNT (MOSAICO_THERMAL_WIDTH * MOSAICO_THERMAL_HEIGHT)

typedef struct mosaico_thermal *mosaico_thermal_handle_t;

typedef struct {
    uint32_t clock_speed_hz;         /**< I2C clock, up to 1 MHz. */
    uint8_t refresh_rate;            /**< Subpage rate: 0=0.5 Hz ... 7=64 Hz. */
    float emissivity;               /**< Finite value in (0, 1]. */
    float reflected_offset_c;       /**< Reflected temperature = ambient - offset. */
} mosaico_thermal_config_t;

#define MOSAICO_THERMAL_DEFAULT_CONFIG() { .clock_speed_hz = 400000, .refresh_rate = 5, .emissivity = 0.95f, .reflected_offset_c = 8.0f }

typedef struct {
    float pixels[MOSAICO_THERMAL_PIXEL_COUNT]; /**< Row-major temperatures in Celsius. */
    float ambient_c;
    float supply_v;
} mosaico_thermal_frame_t;

/**
 * @brief Initialize an MLX90640 in a claimed Mosaico slot and load calibration.
 * Initialize and claim the slot through mosaico_module_mgr before calling.
 * Only one thermal board may be connected: both slots share address 0x33.
 * Pass NULL for the default configuration. Serialize all handle operations,
 * and destroy the sensor before releasing the slot.
 * On failure, out_sensor is NULL unless device removal also fails; in that case
 * only retry deletion on the returned handle. Initialization uses a 2-second I/O deadline.
 */
esp_err_t mosaico_thermal_new(bsp_subboard_slot_t slot, const mosaico_thermal_config_t *config, mosaico_thermal_handle_t *out_sensor);

/**
 * @brief Read both chess subpages and correct bad pixels; output changes only on success.
 * timeout_ms must be positive and covers polling and I/O for both subpages.
 * Allow at least two subpage periods plus transfer/calculation time. Scheduling
 * and temperature calculation can extend wall time beyond the I/O deadline.
 */
esp_err_t mosaico_thermal_read(mosaico_thermal_handle_t sensor, mosaico_thermal_frame_t *frame, int timeout_ms);

/** @brief Remove the device, then free the handle. On failure the handle remains valid. */
esp_err_t mosaico_thermal_del(mosaico_thermal_handle_t sensor);

#ifdef __cplusplus
}
#endif
