/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_button_led.h"

#include <stdlib.h>
#include <string.h>

#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

static const char *TAG = "mosaico_btn_led";

struct mosaico_button_led_t {
    mosaico_button_led_config_t config;
    mosaico_module_mgr_slot_t slot;
    bsp_subboard_button_led_config_t hardware;
    led_strip_handle_t strip;
    SemaphoreHandle_t lock;
    bool subboard_claimed;
    mosaico_button_led_color_t colors[MOSAICO_BUTTON_LED_COUNT];
};

static uint8_t scale_channel(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * brightness) / 255U);
}

static esp_err_t configure_keys(const bsp_subboard_button_led_config_t *hardware)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(hardware->key1_io) | BIT64(hardware->key2_io),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static esp_err_t create_led_strip(mosaico_button_led_handle_t handle)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = handle->hardware.ws2812_io,
        .max_leds = handle->hardware.led_count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = {
            .with_dma = false,
        },
    };
    return led_strip_new_rmt_device(&strip_config, &rmt_config, &handle->strip);
}

static esp_err_t push_leds(mosaico_button_led_handle_t handle)
{
    for (uint8_t i = 0; i < handle->hardware.led_count; ++i) {
        const mosaico_button_led_color_t color = handle->colors[i];
        ESP_RETURN_ON_ERROR(
            led_strip_set_pixel(handle->strip, i,
                                scale_channel(color.r, handle->config.led_brightness),
                                scale_channel(color.g, handle->config.led_brightness),
                                scale_channel(color.b, handle->config.led_brightness)),
            TAG, "set LED %u failed", i);
    }
    return led_strip_refresh(handle->strip);
}

static void release_resources(mosaico_button_led_handle_t handle)
{
    if (!handle) {
        return;
    }

    if (handle->strip) {
        led_strip_clear(handle->strip);
        led_strip_del(handle->strip);
        handle->strip = NULL;
    }

    if (handle->subboard_claimed) {
        esp_err_t ret = mosaico_module_mgr_release(handle->slot);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Release subboard slot %s failed: %s",
                     mosaico_module_mgr_slot_to_name(handle->slot),
                     esp_err_to_name(ret));
        }
        handle->subboard_claimed = false;
    }
}

esp_err_t mosaico_button_led_new(const mosaico_button_led_config_t *config,
                                 mosaico_button_led_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG,
                        "button LED output handle is null");
    *out_handle = NULL;

    mosaico_button_led_config_t active =
        config ? *config
               : (mosaico_button_led_config_t)MOSAICO_BUTTON_LED_DEFAULT_CONFIG();
    ESP_RETURN_ON_FALSE(
        active.discovery_timeout_ms > 0 &&
            active.led_brightness > 0 &&
            (active.slot == MOSAICO_MODULE_MGR_SLOT_AUTO ||
             active.slot < MOSAICO_MODULE_MGR_SLOT_COUNT),
        ESP_ERR_INVALID_ARG, TAG, "invalid button LED configuration");

    ESP_RETURN_ON_ERROR(mosaico_module_mgr_init(NULL), TAG,
                        "initialize module manager failed");

    mosaico_module_mgr_info_t module_info = {0};
    esp_err_t ret = mosaico_module_mgr_wait_for(
        MOSAICO_BOARD_TYPE_BUTTON_LED, active.slot,
        active.discovery_timeout_ms, &module_info);
    ESP_RETURN_ON_ERROR(ret, TAG, "discover button LED subboard failed");

    mosaico_button_led_handle_t handle =
        heap_caps_calloc(1, sizeof(*handle),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG,
                        "allocate button LED context failed");
    handle->config = active;
    handle->slot = module_info.slot;
    /*
     * Prefer the probed AT24C02 address as the slot identity, then map pins
     * from that slot (0x50 left / 0x51 right).
     */
    if (module_info.eeprom_addr != 0) {
        bsp_subboard_slot_t addr_slot = BSP_SUBBOARD_SLOT_LEFT;
        ret = bsp_subboard_slot_from_eeprom_addr(module_info.eeprom_addr, &addr_slot);
        if (ret != ESP_OK) {
            heap_caps_free(handle);
            ESP_LOGE(TAG, "Map EEPROM 0x%02X to slot failed: %s",
                     module_info.eeprom_addr, esp_err_to_name(ret));
            return ret;
        }
        if ((mosaico_module_mgr_slot_t)addr_slot != module_info.slot) {
            ESP_LOGW(TAG,
                     "Slot/address mismatch: slot=%s eeprom=0x%02X mapped=%d; using address",
                     mosaico_module_mgr_slot_to_name(module_info.slot),
                     module_info.eeprom_addr, addr_slot);
        }
        handle->slot = (mosaico_module_mgr_slot_t)addr_slot;
    }

    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        heap_caps_free(handle);
        ESP_LOGE(TAG, "Create button LED mutex failed: %s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    ret = mosaico_module_mgr_claim(handle->slot, MOSAICO_BOARD_TYPE_BUTTON_LED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Claim button LED subboard failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }
    handle->subboard_claimed = true;

    ret = bsp_subboard_button_led_get_config(
        (bsp_subboard_slot_t)handle->slot, &handle->hardware);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Resolve button LED pins failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = configure_keys(&handle->hardware);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Configure key GPIOs failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = create_led_strip(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create WS2812 strip failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = led_strip_clear(handle->strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Clear WS2812 strip failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ESP_LOGI(TAG,
             "Button LED ready: slot=%s eeprom=0x%02X KEY1=%d KEY2=%d WS2812=%d",
             mosaico_module_mgr_slot_to_name(handle->slot),
             handle->hardware.eeprom_addr, handle->hardware.key1_io,
             handle->hardware.key2_io, handle->hardware.ws2812_io);
    *out_handle = handle;
    return ESP_OK;

fail:
    release_resources(handle);
    vSemaphoreDelete(handle->lock);
    heap_caps_free(handle);
    return ret;
}

esp_err_t mosaico_button_led_get_info(mosaico_button_led_handle_t handle,
                                      mosaico_button_led_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(handle && out_info, ESP_ERR_INVALID_ARG, TAG,
                        "invalid button LED info request");

    bool key1 = false;
    bool key2 = false;
    ESP_RETURN_ON_ERROR(mosaico_button_led_read_keys(handle, &key1, &key2), TAG,
                        "read keys for info failed");

    out_info->slot = handle->slot;
    out_info->eeprom_addr = handle->hardware.eeprom_addr;
    out_info->key1_pressed = key1;
    out_info->key2_pressed = key2;
    out_info->led_count = handle->hardware.led_count;
    return ESP_OK;
}

esp_err_t mosaico_button_led_read_keys(mosaico_button_led_handle_t handle,
                                       bool *key1_pressed,
                                       bool *key2_pressed)
{
    ESP_RETURN_ON_FALSE(handle && key1_pressed && key2_pressed,
                        ESP_ERR_INVALID_ARG, TAG, "invalid key read request");

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Take button LED lock failed: %s",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }

    /* Active-low keys with internal pull-ups. */
    *key1_pressed = gpio_get_level(handle->hardware.key1_io) == 0;
    *key2_pressed = gpio_get_level(handle->hardware.key2_io) == 0;
    xSemaphoreGive(handle->lock);

    ESP_LOGD(TAG, "Keys: KEY1=%d KEY2=%d", *key1_pressed, *key2_pressed);
    return ESP_OK;
}

esp_err_t mosaico_button_led_set_led(mosaico_button_led_handle_t handle,
                                     uint8_t index,
                                     mosaico_button_led_color_t color)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle is null");
    ESP_RETURN_ON_FALSE(index < handle->hardware.led_count, ESP_ERR_INVALID_ARG,
                        TAG, "LED index %u out of range", index);

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Take button LED lock failed: %s",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }

    handle->colors[index] = color;
    esp_err_t ret = push_leds(handle);
    xSemaphoreGive(handle->lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Update LED %u failed: %s", index, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mosaico_button_led_set_all(mosaico_button_led_handle_t handle,
                                     mosaico_button_led_color_t color)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle is null");

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Take button LED lock failed: %s",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }

    for (uint8_t i = 0; i < handle->hardware.led_count; ++i) {
        handle->colors[i] = color;
    }
    esp_err_t ret = push_leds(handle);
    xSemaphoreGive(handle->lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Update all LEDs failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mosaico_button_led_clear(mosaico_button_led_handle_t handle)
{
    const mosaico_button_led_color_t off = {0, 0, 0};
    return mosaico_button_led_set_all(handle, off);
}

esp_err_t mosaico_button_led_refresh(mosaico_button_led_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle is null");

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Take button LED lock failed: %s",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = push_leds(handle);
    xSemaphoreGive(handle->lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Refresh LEDs failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t mosaico_button_led_del(mosaico_button_led_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle is null");

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Take button LED lock failed: %s",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }

    const mosaico_module_mgr_slot_t slot = handle->slot;
    release_resources(handle);
    xSemaphoreGive(handle->lock);
    vSemaphoreDelete(handle->lock);
    heap_caps_free(handle);

    ESP_LOGI(TAG, "Button LED closed; slot=%s discovery resumed",
             mosaico_module_mgr_slot_to_name(slot));
    return ESP_OK;
}
