/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Left/right expansion discrimination:
 *
 * Both slots use SDA=GPIO0 and SCL=GPIO1. This is shared with mainboard I2C0
 * on v1.0 and driven by dedicated I2C1 on v1.2. Standard modules use the slot
 * control GPIO as AT24C02 A0 so the two EEPROMs appear at different addresses:
 *
 *   left  : GPIO14 = 0 -> AT24C02 @ 0x50
 *   right : GPIO39 = 1 -> AT24C02 @ 0x51
 *
 * The camera EEPROM is internally fixed at 0x50 and has no address line;
 * GPIO14 is camera D4. Applications discover boards by probing the configured
 * addresses, then map connector GPIOs with bsp_subboard_map_gpio().
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

/**
 * @brief Prepare the two Mosaico subboard slots for EEPROM discovery
 *
 * Enables the shared subboard rails and I2C bus, then applies the standard
 * module discovery levels for the left and right slots.
 */
esp_err_t bsp_subboard_init(void);

/** @brief Return the initialized subboard I2C bus, or NULL before initialization. */
i2c_master_bus_handle_t bsp_subboard_get_i2c_bus(void);

esp_err_t bsp_subboard_get_slot_config(bsp_subboard_slot_t slot,
                                       bsp_subboard_slot_config_t *out_config);

/**
 * @brief Restore one slot's control GPIO to its discovery level
 *
 * Call after a driver has temporarily reused the address-select GPIO.
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
 * Covers the six H-connector pins plus the extended left/right pairs from the
 * IO loopback fixture:
 *   16<->40, 15<->38, 17<->37, 18<->54, 19<->52, 55<->49, 4<->5, ...
 *
 * Unmapped GPIOs are returned unchanged.
 */
gpio_num_t bsp_subboard_map_gpio(bsp_subboard_slot_t slot, gpio_num_t left_gpio);

#ifdef __cplusplus
}
#endif
