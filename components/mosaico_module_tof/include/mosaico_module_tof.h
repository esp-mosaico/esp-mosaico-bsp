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
 */
esp_err_t mosaico_tof_new(bsp_subboard_slot_t slot, vl53l0x_handle_t *out_sensor);

#ifdef __cplusplus
}
#endif
