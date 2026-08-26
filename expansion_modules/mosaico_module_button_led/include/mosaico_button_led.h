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

#define MOSAICO_BUTTON_LED_COUNT           3U

#define MOSAICO_BUTTON_LED_DEFAULT_CONFIG() {            \
    .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,                  \
    .discovery_timeout_ms = 1500,                        \
    .led_brightness = 32,                                \
}

typedef struct mosaico_button_led_t *mosaico_button_led_handle_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    uint32_t discovery_timeout_ms;
    uint8_t led_brightness;
} mosaico_button_led_config_t;

typedef struct {
    mosaico_module_mgr_slot_t slot;
    uint8_t eeprom_addr;
    bool key1_pressed;
    bool key2_pressed;
    uint8_t led_count;
} mosaico_button_led_info_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} mosaico_button_led_color_t;

/**
 * @brief Discover, claim and open the button/LED subboard
 *
 * Pass NULL for automatic defaults. The slot is selected from the AT24C02
 * address reported by mosaico_module_mgr (0x50 left / 0x51 right).
 */
esp_err_t mosaico_button_led_new(const mosaico_button_led_config_t *config,
                                 mosaico_button_led_handle_t *out_handle);

esp_err_t mosaico_button_led_get_info(mosaico_button_led_handle_t handle,
                                      mosaico_button_led_info_t *out_info);

esp_err_t mosaico_button_led_read_keys(mosaico_button_led_handle_t handle,
                                       bool *key1_pressed,
                                       bool *key2_pressed);

esp_err_t mosaico_button_led_set_led(mosaico_button_led_handle_t handle,
                                     uint8_t index,
                                     mosaico_button_led_color_t color);

esp_err_t mosaico_button_led_set_all(mosaico_button_led_handle_t handle,
                                     mosaico_button_led_color_t color);

esp_err_t mosaico_button_led_clear(mosaico_button_led_handle_t handle);

esp_err_t mosaico_button_led_refresh(mosaico_button_led_handle_t handle);

/**
 * @brief Release GPIOs and return the slot to EEPROM discovery
 */
esp_err_t mosaico_button_led_del(mosaico_button_led_handle_t handle);

#ifdef __cplusplus
}
#endif
