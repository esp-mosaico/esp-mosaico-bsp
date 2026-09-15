/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/subboard.h"

#include <stddef.h>

#include "bsp/esp_mosaico.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bsp_subboard";

typedef struct {
    gpio_num_t left;
    gpio_num_t right;
} bsp_subboard_gpio_pair_t;

static const bsp_subboard_slot_config_t s_slot_configs[BSP_SUBBOARD_SLOT_COUNT] = {
    [BSP_SUBBOARD_SLOT_LEFT] = {
        .slot = BSP_SUBBOARD_SLOT_LEFT,
        .eeprom_addr = BSP_SUBBOARD_EEPROM_ADDR_LEFT,
        .address_select_gpio = BSP_SUBBOARD_ADDR_GPIO_LEFT,
        .address_select_level = BSP_SUBBOARD_ADDR_LEVEL_LEFT,
        .connector_gpio = {
            GPIO_NUM_53, GPIO_NUM_48, GPIO_NUM_13,
            GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_4,
        },
        .rotated_180 = false,
    },
    [BSP_SUBBOARD_SLOT_RIGHT] = {
        .slot = BSP_SUBBOARD_SLOT_RIGHT,
        .eeprom_addr = BSP_SUBBOARD_EEPROM_ADDR_RIGHT,
        .address_select_gpio = BSP_SUBBOARD_ADDR_GPIO_RIGHT,
        .address_select_level = BSP_SUBBOARD_ADDR_LEVEL_RIGHT,
        .connector_gpio = {
            GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_11,
            GPIO_NUM_10, GPIO_NUM_39, GPIO_NUM_5,
        },
        .rotated_180 = true,
    },
};

/*
 * Left-slot canonical GPIOs and their right-slot mirrors.
 * H-connector pairs come from the BSP slot tables; extended pairs come from
 * the dual-slot IO loopback fixture used on the CoreBoard.
 */
static const bsp_subboard_gpio_pair_t s_gpio_pairs[] = {
    {GPIO_NUM_53, GPIO_NUM_46}, /* H2 */
    {GPIO_NUM_48, GPIO_NUM_47}, /* H4 */
    {GPIO_NUM_13, GPIO_NUM_11}, /* H6 */
    {GPIO_NUM_12, GPIO_NUM_10}, /* H8 */
    {GPIO_NUM_14, GPIO_NUM_39}, /* H10 / standard module EEPROM A0 */
    {GPIO_NUM_4,  GPIO_NUM_5},  /* H12 */
    {GPIO_NUM_16, GPIO_NUM_40},
    {GPIO_NUM_15, GPIO_NUM_38},
    {GPIO_NUM_17, GPIO_NUM_37},
    {GPIO_NUM_18, GPIO_NUM_54},
    {GPIO_NUM_19, GPIO_NUM_52},
    {GPIO_NUM_55, GPIO_NUM_49},
};

static portMUX_TYPE s_resource_lock = portMUX_INITIALIZER_UNLOCKED;
static i2c_master_bus_handle_t s_i2c_bus;
static bool s_subboard_initialized;

static esp_err_t init_i2c_bus(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }

    bsp_board_variant_t variant = BSP_BOARD_VARIANT_V1_0;
    ESP_RETURN_ON_ERROR(bsp_board_variant_get(&variant), TAG, "detect board variant failed");
    if (variant == BSP_BOARD_VARIANT_V1_0) {
        ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "initialize shared v1.0 I2C bus failed");
        s_i2c_bus = bsp_i2c_get_handle();
        ESP_RETURN_ON_FALSE(s_i2c_bus, ESP_ERR_INVALID_STATE, TAG, "shared v1.0 I2C bus is null");
        return ESP_OK;
    }

    const i2c_master_bus_config_t config = {
        .i2c_port = BSP_SUBBOARD_I2C_PORT_V1_2,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_SUBBOARD_I2C_SDA,
        .scl_io_num = BSP_SUBBOARD_I2C_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_i2c_bus), TAG, "create subboard I2C bus failed");
    ESP_LOGI(TAG, "Subboard I2C initialized: port=%d SDA=%d SCL=%d", BSP_SUBBOARD_I2C_PORT_V1_2,
             BSP_SUBBOARD_I2C_SDA, BSP_SUBBOARD_I2C_SCL);
    return ESP_OK;
}

static esp_err_t configure_address_select(const bsp_subboard_slot_config_t *slot)
{
    /* Camera D4 reuses GPIO14; restore the standard module discovery function. */
    ESP_RETURN_ON_ERROR(gpio_reset_pin(slot->address_select_gpio), TAG,
                        "reset slot %d address GPIO failed", slot->slot);
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(slot->address_select_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(slot->address_select_gpio, slot->address_select_level),
                        TAG, "preset slot %d address GPIO failed", slot->slot);
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG,
                        "configure slot %d address GPIO failed", slot->slot);
    ESP_LOGD(TAG, "Address select: slot=%d GPIO%d=%u -> EEPROM 0x%02X",
             slot->slot, slot->address_select_gpio, slot->address_select_level,
             slot->eeprom_addr);
    return ESP_OK;
}

esp_err_t bsp_subboard_init(void)
{
    portENTER_CRITICAL(&s_resource_lock);
    const bool initialized = s_subboard_initialized;
    portEXIT_CRITICAL(&s_resource_lock);
    if (initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG,
                        "enable subboard VCC rail failed");
    ESP_RETURN_ON_ERROR(init_i2c_bus(), TAG, "initialize subboard I2C failed");

    for (size_t i = 0; i < BSP_SUBBOARD_SLOT_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(configure_address_select(&s_slot_configs[i]), TAG,
                            "initialize slot %u failed", (unsigned)i);
    }

    portENTER_CRITICAL(&s_resource_lock);
    s_subboard_initialized = true;
    portEXIT_CRITICAL(&s_resource_lock);
    ESP_LOGI(TAG,
             "Subboard address select ready: left GPIO%d=%d -> 0x%02X, "
             "right GPIO%d=%d -> 0x%02X",
             BSP_SUBBOARD_ADDR_GPIO_LEFT, BSP_SUBBOARD_ADDR_LEVEL_LEFT,
             BSP_SUBBOARD_EEPROM_ADDR_LEFT,
             BSP_SUBBOARD_ADDR_GPIO_RIGHT, BSP_SUBBOARD_ADDR_LEVEL_RIGHT,
             BSP_SUBBOARD_EEPROM_ADDR_RIGHT);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_subboard_get_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t bsp_subboard_get_slot_config(bsp_subboard_slot_t slot,
                                       bsp_subboard_slot_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config && slot >= BSP_SUBBOARD_SLOT_LEFT &&
                            slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid slot config request");
    *out_config = s_slot_configs[slot];
    return ESP_OK;
}

esp_err_t bsp_subboard_apply_address_select(bsp_subboard_slot_t slot)
{
    ESP_RETURN_ON_FALSE(slot >= BSP_SUBBOARD_SLOT_LEFT &&
                            slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid address-select slot");

    ESP_RETURN_ON_ERROR(configure_address_select(&s_slot_configs[slot]), TAG,
                        "apply address select for slot %d failed", slot);
    ESP_LOGI(TAG, "Address select applied: slot=%d GPIO%d=%u EEPROM=0x%02X",
             slot, s_slot_configs[slot].address_select_gpio,
             s_slot_configs[slot].address_select_level,
             s_slot_configs[slot].eeprom_addr);
    return ESP_OK;
}

esp_err_t bsp_subboard_slot_from_eeprom_addr(uint8_t eeprom_addr,
                                             bsp_subboard_slot_t *out_slot)
{
    ESP_RETURN_ON_FALSE(out_slot, ESP_ERR_INVALID_ARG, TAG,
                        "slot output is null");
    if (eeprom_addr == BSP_SUBBOARD_EEPROM_ADDR_LEFT) {
        *out_slot = BSP_SUBBOARD_SLOT_LEFT;
        return ESP_OK;
    }
    if (eeprom_addr == BSP_SUBBOARD_EEPROM_ADDR_RIGHT) {
        *out_slot = BSP_SUBBOARD_SLOT_RIGHT;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Unknown subboard EEPROM address 0x%02X: %s", eeprom_addr,
             esp_err_to_name(ESP_ERR_NOT_FOUND));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t bsp_subboard_eeprom_addr_from_slot(bsp_subboard_slot_t slot,
                                             uint8_t *out_addr)
{
    ESP_RETURN_ON_FALSE(out_addr && slot >= BSP_SUBBOARD_SLOT_LEFT &&
                            slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid EEPROM address request");
    *out_addr = s_slot_configs[slot].eeprom_addr;
    return ESP_OK;
}

gpio_num_t bsp_subboard_map_gpio(bsp_subboard_slot_t slot, gpio_num_t left_gpio)
{
    if (slot != BSP_SUBBOARD_SLOT_RIGHT) {
        return left_gpio;
    }

    for (size_t i = 0; i < sizeof(s_gpio_pairs) / sizeof(s_gpio_pairs[0]); ++i) {
        if (s_gpio_pairs[i].left == left_gpio) {
            return s_gpio_pairs[i].right;
        }
    }
    return left_gpio;
}
