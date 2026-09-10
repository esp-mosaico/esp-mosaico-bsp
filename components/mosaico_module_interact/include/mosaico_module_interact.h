/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_module_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_INTERACT_LED_COUNT 6U

#define MOSAICO_INTERACT_DEFAULT_CONFIG() {              \
    .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,                 \
    .discovery_timeout_ms = 1500,                        \
    .button_mode = MOSAICO_INTERACT_BUTTON_MODE_AUTO,     \
    .led_brightness = 32,                                \
}

typedef struct mosaico_interact_t *mosaico_interact_handle_t;

typedef enum {
    MOSAICO_INTERACT_BUTTON_MODE_AUTO = 0, // Prefer touch; fall back to GPIO.
    MOSAICO_INTERACT_BUTTON_MODE_GPIO,
    MOSAICO_INTERACT_BUTTON_MODE_TOUCH,
} mosaico_interact_button_mode_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    uint32_t discovery_timeout_ms;
    mosaico_interact_button_mode_t button_mode;
    uint8_t led_brightness; // 0–255; zero disables light output.
} mosaico_interact_config_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    mosaico_interact_button_mode_t button_mode; // Actual mode; never AUTO.
    uint8_t led_count;
} mosaico_interact_info_t;

typedef struct {
    bool left_pressed;
    bool right_pressed;
    bool motion_detected;
    int light_raw;
    uint8_t light_level; // Filtered relative level, 0–100; not lux.
} mosaico_interact_inputs_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} mosaico_interact_rgb_t;

/**
 * Discover, claim and initialize the interaction board. NULL config uses defaults.
 * The slot follows module manager discovery.
 * Touch detects both fingers and mechanical key-to-GND presses on shared pads.
 * Explicit GPIO/TOUCH modes never fall back. Input or LED initialization failure
 * releases acquired resources and leaves *out_handle NULL. IR is allocated lazily.
 */
esp_err_t mosaico_interact_open(const mosaico_interact_config_t *config, mosaico_interact_handle_t *out_handle);

/** Stop all concurrent users before closing; set the caller's handle to NULL afterward. */
esp_err_t mosaico_interact_close(mosaico_interact_handle_t handle);

/** Return immutable device information without sampling inputs. */
esp_err_t mosaico_interact_get_info(mosaico_interact_handle_t handle, mosaico_interact_info_t *out_info);

/** Sample inputs sequentially; no edge events or simultaneous sampling. Output is unchanged on error. */
esp_err_t mosaico_interact_read_inputs(mosaico_interact_handle_t handle, mosaico_interact_inputs_t *out_inputs);

/* LED operations are serialized and refresh immediately. Success commits the cached
 * colors; transmission failure may leave hardware uncertain until the next full frame.
 * Buffer pointers are not retained after return. These APIs are not ISR-safe.
 */

/** Set one zero-based LED, preserving the other colors. */
esp_err_t mosaico_interact_led_set(mosaico_interact_handle_t handle, uint8_t index, mosaico_interact_rgb_t color);

/** Replace the entire frame; count must equal led_count and colors must not be NULL. */
esp_err_t mosaico_interact_led_write(mosaico_interact_handle_t handle, const mosaico_interact_rgb_t *colors, size_t count);

/** Set every LED to the same color. */
esp_err_t mosaico_interact_led_fill(mosaico_interact_handle_t handle, mosaico_interact_rgb_t color);

/** Set every LED to black, equivalent to led_fill(handle, black). */
esp_err_t mosaico_interact_led_clear(mosaico_interact_handle_t handle);

/**
 * Send one standard NEC frame at 38 kHz and wait for completion.
 * The driver appends inverses of the 8-bit address and command.
 * IR RMT is allocated on first send so both boards can initialize WS2812 channels
 * on ESP32-S31 (4 TX channels total); allocation or transmission can fail.
 */
esp_err_t mosaico_interact_ir_send_nec(mosaico_interact_handle_t handle, uint8_t address, uint8_t command);

#ifdef __cplusplus
}
#endif
