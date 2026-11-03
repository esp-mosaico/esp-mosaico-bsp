/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "bsp/subboard.h"
#include "vl53l0x.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mosaico_tof *mosaico_tof_handle_t;

/* Common distance, validity, signal, and ambient fields for both sensor models. */
typedef vl53l0x_data_t mosaico_tof_data_t;

/**
 * @brief Reset, identify, and initialize the default VL53L4CD or compatible VL53L0X module.
 *
 * Default hardware: VL53L4CDV0DH/1. Compatible hardware: VL53L0CXV0DH/1.
 * The silicon ID automatically selects model-specific initialization and reads.
 * Initialize the module manager and claim a ToF slot first. Only one sensor at
 * address 0x29 may be connected. On failure, out_sensor is NULL. Serialize all
 * operations and close the handle before releasing the slot.
 */
esp_err_t mosaico_tof_open(bsp_subboard_slot_t slot, mosaico_tof_handle_t *out_sensor);

/** Read a sample, waiting at most 500 ms for VL53L4CD data-ready.
 * ESP_OK reports communication success; use the distance only when data.valid.
 * VL53L0X uses single-shot ranging; VL53L4CD uses continuous 33 ms ranging.
 */
esp_err_t mosaico_tof_read(mosaico_tof_handle_t sensor, mosaico_tof_data_t *data);

/** Stop ranging and free resources. A failed removal of host resources retains
 * the handle for retry. Shared power and the I2C bus remain available.
 */
esp_err_t mosaico_tof_close(mosaico_tof_handle_t sensor);

/** Return the detected model name, or "UNKNOWN" for a NULL handle. */
const char *mosaico_tof_model_name(mosaico_tof_handle_t sensor);

/**
 * @brief Reset and initialize one VL53L0X module for single measurements.
 *
 * Initialize and claim the slot through mosaico_module_mgr before calling.
 * Only one ToF board may be connected: both slots share address 0x29.
 * Serialize access to the handle; read with vl53l0x_single_measure() and
 * destroy with vl53l0x_destroy() before releasing the slot.
 * On failure, out_sensor is NULL.
 * This compatibility API supports VL53L0X only; use mosaico_tof_open() for
 * automatic VL53L0X/VL53L4CD selection.
 */
esp_err_t mosaico_tof_new(bsp_subboard_slot_t slot, vl53l0x_handle_t *out_sensor);

#ifdef __cplusplus
}
#endif
