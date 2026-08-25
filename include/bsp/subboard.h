/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Left/right expansion discrimination:
 *
 * Both slots share I2C0 (SDA=GPIO0, SCL=GPIO1). The mainboard drives each
 * slot's AT24C02 A0 pin to a fixed level so the two EEPROMs appear at
 * different 7-bit addresses:
 *
 *   left  : GPIO14 = 0 -> AT24C02 @ 0x50
 *   right : GPIO39 = 1 -> AT24C02 @ 0x51
 *
 * Applications discover boards by probing those addresses, then map connector
 * GPIOs with bsp_subboard_map_gpio() using the resolved slot.
 */

#define BSP_SUBBOARD_EEPROM_ADDR_LEFT     0x50U
#define BSP_SUBBOARD_EEPROM_ADDR_RIGHT    0x51U
#define BSP_SUBBOARD_ADDR_GPIO_LEFT       GPIO_NUM_14
#define BSP_SUBBOARD_ADDR_GPIO_RIGHT      GPIO_NUM_39
#define BSP_SUBBOARD_ADDR_LEVEL_LEFT      0
#define BSP_SUBBOARD_ADDR_LEVEL_RIGHT     1

typedef enum {
    BSP_SUBBOARD_SLOT_LEFT = 0,
    BSP_SUBBOARD_SLOT_RIGHT,
    BSP_SUBBOARD_SLOT_COUNT,
} bsp_subboard_slot_t;

typedef struct {
    bsp_subboard_slot_t slot;
    uint8_t eeprom_addr;
    gpio_num_t address_select_gpio;
    uint8_t address_select_level;
    gpio_num_t connector_gpio[6];
    bool rotated_180;
} bsp_subboard_slot_config_t;

typedef struct {
    gpio_num_t data_io[8];
    gpio_num_t vsync_io;
    gpio_num_t de_io;
    gpio_num_t pclk_io;
    gpio_num_t xclk_io;
    gpio_num_t reset_io;
    gpio_num_t pwdn_io;
    gpio_num_t flash_io;
    uint32_t xclk_freq_hz;
    uint32_t xclk_stabilization_ms;
    uint32_t sccb_freq_hz;
} bsp_subboard_camera_config_t;

typedef struct {
    gpio_num_t key1_io;
    gpio_num_t key2_io;
    gpio_num_t ws2812_io;
    uint8_t led_count;
    uint8_t eeprom_addr;
    bool rotated_180;
} bsp_subboard_button_led_config_t;

#define BSP_SUBBOARD_JOYSTICK_BUTTON_COUNT 5U

typedef struct {
    gpio_num_t io;
    bool active_high;
} bsp_subboard_joystick_button_config_t;

typedef struct {
    gpio_num_t x_io;
    gpio_num_t y_io;
    bool invert_x;
    bool invert_y;
    bsp_subboard_joystick_button_config_t buttons[BSP_SUBBOARD_JOYSTICK_BUTTON_COUNT];
    uint8_t eeprom_addr;
    bool rotated_180;
} bsp_subboard_joystick_config_t;

/**
 * @brief Prepare the two Mosaico subboard slots for EEPROM discovery
 *
 * Enables the shared subboard rails and I2C bus, then drives the EEPROM
 * address-select pins so the left and right slots appear at 0x50 and 0x51.
 */
esp_err_t bsp_subboard_init(void);

esp_err_t bsp_subboard_get_slot_config(bsp_subboard_slot_t slot,
                                       bsp_subboard_slot_config_t *out_config);

/**
 * @brief Drive one slot's AT24C02 A0 pin to its discovery level
 *
 * Call after a driver has temporarily reused the address-select GPIO
 * (for example camera D4 on GPIO14).
 */
esp_err_t bsp_subboard_apply_address_select(bsp_subboard_slot_t slot);

/**
 * @brief Resolve slot from the probed AT24C02 7-bit address
 */
esp_err_t bsp_subboard_slot_from_eeprom_addr(uint8_t eeprom_addr,
                                             bsp_subboard_slot_t *out_slot);

/**
 * @brief Return the AT24C02 7-bit address owned by a slot
 */
esp_err_t bsp_subboard_eeprom_addr_from_slot(bsp_subboard_slot_t slot,
                                             uint8_t *out_addr);

/**
 * @brief Map a left-slot canonical GPIO onto the active slot
 *
 * Covers the six H-connector pins plus the extended left/right pairs used by
 * camera-class and button/LED boards (from the IO loopback fixture):
 *   16<->40, 15<->38, 17<->37, 18<->54, 19<->52, 55<->49, 4<->5, ...
 *
 * Unmapped GPIOs are returned unchanged.
 */
gpio_num_t bsp_subboard_map_gpio(bsp_subboard_slot_t slot, gpio_num_t left_gpio);

/**
 * @brief Claim the DVP camera wiring and return its fixed hardware description
 *
 * The current camera subboard is supported in the left slot only. Claiming it
 * repurposes the left EEPROM address-select GPIO as DVP D4 and suspends EEPROM
 * access to that slot until bsp_subboard_camera_release() is called.
 */
esp_err_t bsp_subboard_camera_acquire(bsp_subboard_slot_t slot,
                                      bsp_subboard_camera_config_t *out_config);

/**
 * @brief Release camera pins and restore EEPROM discovery on the slot
 */
esp_err_t bsp_subboard_camera_release(bsp_subboard_slot_t slot);

/**
 * @brief Resolve button/LED subboard GPIOs for a discovered slot
 *
 * Left-slot absolute pins are KEY1=GPIO12, KEY2=GPIO15, WS2812=GPIO4.
 * Right-slot pins are produced by bsp_subboard_map_gpio() (KEY1=GPIO10, ...).
 */
esp_err_t bsp_subboard_button_led_get_config(bsp_subboard_slot_t slot,
                                             bsp_subboard_button_led_config_t *out_config);

/**
 * @brief Resolve joystick ADC and button GPIOs for a discovered slot
 *
 * Normalized X/Y directions match between the left and right slots. Button
 * order is B, 1, 2, 3, 4.
 */
esp_err_t bsp_subboard_joystick_get_config(bsp_subboard_slot_t slot,
                                           bsp_subboard_joystick_config_t *out_config);

#ifdef __cplusplus
}
#endif
