/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_module_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_JOYSTICK_BUTTON_COUNT 5U

#define MOSAICO_JOYSTICK_DEFAULT_CONFIG() { \
    .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,     \
    .discovery_timeout_ms = 1500,           \
    .oversample = 4,                        \
    .deadzone = 0.08f,                      \
    .circle_ms = 8000,                      \
    .center_ms = 1500,                      \
    .center_max_ms = 4500,                  \
    .stable_span = 60,                      \
    .center_stable_span = 120,              \
    .idle_recenter_ms = 1500,               \
    .min_range_warning = 80,                \
    .min_span_finish = 40,                  \
}

typedef struct mosaico_joystick_t *mosaico_joystick_handle_t;

typedef enum {
    MOSAICO_JOYSTICK_CAL_CIRCLE = 0,
    MOSAICO_JOYSTICK_CAL_CENTER,
    MOSAICO_JOYSTICK_READY,
} mosaico_joystick_state_t;

typedef enum {
    MOSAICO_JOYSTICK_BUTTON_B = 0,
    MOSAICO_JOYSTICK_BUTTON_1,
    MOSAICO_JOYSTICK_BUTTON_2,
    MOSAICO_JOYSTICK_BUTTON_3,
    MOSAICO_JOYSTICK_BUTTON_4,
} mosaico_joystick_button_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    uint32_t discovery_timeout_ms;
    uint8_t oversample;
    float deadzone;
    uint32_t circle_ms;
    uint32_t center_ms;
    uint32_t center_max_ms;
    int stable_span;
    int center_stable_span;
    uint32_t idle_recenter_ms;
    int min_range_warning;
    int min_span_finish;
} mosaico_joystick_config_t;

typedef struct {
    int min_x;
    int max_x;
    int center_x;
    int min_y;
    int max_y;
    int center_y;
} mosaico_joystick_calibration_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    mosaico_joystick_state_t state;
    int raw_x;
    int raw_y;
    float x;
    float y;
    bool buttons[MOSAICO_JOYSTICK_BUTTON_COUNT];
    mosaico_joystick_calibration_t calibration;
} mosaico_joystick_data_t;

/**
 * @brief Discover and claim one programmed Joystick Board
 *
 * The module manager must identify the EEPROM as
 * MOSAICO_BOARD_TYPE_HANDLE before this succeeds.
 * Pass NULL for the default configuration.
 */
esp_err_t mosaico_joystick_new(const mosaico_joystick_config_t *config,
                               mosaico_joystick_handle_t *out_handle);

/**
 * @brief Sample the joystick and return its current state
 *
 * This advances the non-blocking calibration state machine and returns raw
 * axes, normalized axes, buttons, and calibration values in one call. Call it
 * periodically from the application task. Returns ESP_ERR_NOT_FOUND if the
 * claimed subboard has been removed.
 */
esp_err_t mosaico_joystick_read(mosaico_joystick_handle_t handle,
                                mosaico_joystick_data_t *out_data);

/**
 * @brief Restart circle and center calibration
 */
esp_err_t mosaico_joystick_recalibrate(mosaico_joystick_handle_t handle);

/**
 * @brief Release ADC/GPIO resources and return the slot to discovery
 */
esp_err_t mosaico_joystick_del(mosaico_joystick_handle_t handle);

#ifdef __cplusplus
}
#endif
