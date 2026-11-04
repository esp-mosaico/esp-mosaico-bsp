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
 * Initialize the module manager and claim a ToF slot first. Opening resets the
 * sensor to 0x29. Hold unused sensors in XSHUT and readdress each opened sensor
 * before opening the next slot. Serialize initialization and all operations on
 * each handle. On failure, out_sensor is NULL. Close before releasing the slot.
 */
esp_err_t mosaico_tof_open(bsp_subboard_slot_t slot, mosaico_tof_handle_t *out_sensor);

/** Read a sample, waiting at most 500 ms for continuous data-ready.
 * ESP_OK reports communication success; use the distance only when data.valid.
 * VL53L0X opens in single-shot mode; VL53L4CD opens in continuous mode.
 */
esp_err_t mosaico_tof_read(mosaico_tof_handle_t sensor, mosaico_tof_data_t *data);

/** Start back-to-back ranging on VL53L0X; VL53L4CD is already continuous.
 * Configure address, timing budget and offset before starting VL53L0X.
 * Both sensors can then acquire concurrently on the shared bus. Idempotent.
 */
esp_err_t mosaico_tof_start_continuous(mosaico_tof_handle_t sensor);

/** Poll one new continuous sample without waiting for measurement completion.
 * ESP_OK with ready=false means no new result; ready=true includes invalid
 * ranges, so also check data.valid. Errors invalidate data and leave ready=false.
 * timestamp_ms records the host's data-ready observation, not optical sync time.
 */
esp_err_t mosaico_tof_read_ready(mosaico_tof_handle_t sensor, mosaico_tof_data_t *data, bool *ready);

/** Stop ranging and free resources. A failed removal of host resources retains
 * the handle for retry. Shared power and the I2C bus remain available.
 */
esp_err_t mosaico_tof_close(mosaico_tof_handle_t sensor);

/** Return the detected model name, or "UNKNOWN" for a NULL handle. */
const char *mosaico_tof_model_name(mosaico_tof_handle_t sensor);

/** Set a unique 7-bit address (0x08..0x77, excluding default 0x29).
 * Call after opening and before releasing the next sensor from XSHUT.
 * Configure VL53L0X before starting continuous mode.
 * On failure, close the handle and reset/reopen it before further use.
 */
esp_err_t mosaico_tof_set_address(mosaico_tof_handle_t sensor, uint8_t address);

/** Change timing budget, retaining validity checks. Units: microseconds.
 * VL53L4CD accepts whole milliseconds in 10000..200000 us; VL53L0X uses
 * the PAL's supported timing range. VL53L4CD ranging restarts after updating.
 * Configure VL53L0X before starting continuous mode.
 */
esp_err_t mosaico_tof_set_timing_budget(mosaico_tof_handle_t sensor, uint32_t budget_us);

/** Read the sensor's signed distance offset in micrometers. */
esp_err_t mosaico_tof_get_offset_calibration(mosaico_tof_handle_t sensor, int32_t *offset_um);

/** Set a distance offset in micrometers (-512000..511000, steps of 250).
 * An application can add per-board compensation to the initial offset.
 * Configure VL53L0X before starting continuous mode.
 */
esp_err_t mosaico_tof_set_offset_calibration(mosaico_tof_handle_t sensor, int32_t offset_um);

/**
 * @brief Reset and initialize one VL53L0X module for single measurements.
 *
 * Initialize and claim the slot through mosaico_module_mgr before calling.
 * This function initializes at default address 0x29. The caller must ensure
 * no other sensor responds there: hold unused sensors in XSHUT and assign a
 * unique address to each initialized sensor before starting the next one.
 * Serialize initialization across slots. This function does not coordinate peers.
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
