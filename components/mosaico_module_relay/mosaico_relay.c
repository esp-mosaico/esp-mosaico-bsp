/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include "mosaico_relay.h"
#include "bsp/power.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mosaico_relay";
static const gpio_num_t s_pins[] = {GPIO_NUM_48, GPIO_NUM_53, GPIO_NUM_13, GPIO_NUM_12};

struct mosaico_relay {
    gpio_num_t pins[MOSAICO_RELAY_CHANNEL_COUNT];
};

esp_err_t mosaico_relay_new(bsp_subboard_slot_t slot, mosaico_relay_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "Missing output handle");
    *out_handle = NULL;
    ESP_RETURN_ON_FALSE(slot >= BSP_SUBBOARD_SLOT_LEFT && slot < BSP_SUBBOARD_SLOT_COUNT, ESP_ERR_INVALID_ARG, TAG, "Invalid slot");
    ESP_RETURN_ON_FALSE(bsp_subboard_get_i2c_bus(), ESP_ERR_INVALID_STATE, TAG, "Initialize the module manager first");
    mosaico_relay_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "Allocation failed");

    esp_err_t ret = ESP_OK;
    unsigned configured = 0;
    for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
        handle->pins[i] = bsp_subboard_map_gpio(slot, s_pins[i]);
        /* Preload the inactive level before enabling the output driver. */
        ret = gpio_set_level(handle->pins[i], 0);
        if (ret != ESP_OK) {
            goto fail;
        }
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << handle->pins[i],
            .mode = GPIO_MODE_OUTPUT,
        };
        ret = gpio_config(&config);
        if (ret != ESP_OK) {
            goto fail;
        }
        ++configured;
    }
    /* Use revision-aware BSP power control; this rail is shared with other modules. */
    ret = bsp_power_set_codec_3v3(true);
    if (ret != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    *out_handle = handle;
    ESP_LOGI(TAG, "Ready: slot=%d, all channels OFF", slot);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Initialization failed: %s", esp_err_to_name(ret));
    for (unsigned i = 0; i < configured; ++i) {
        gpio_set_level(handle->pins[i], 0);
    }
    free(handle);
    return ret;
}

esp_err_t mosaico_relay_set(mosaico_relay_handle_t handle, unsigned channel, bool on)
{
    ESP_RETURN_ON_FALSE(handle && channel < MOSAICO_RELAY_CHANNEL_COUNT, ESP_ERR_INVALID_ARG, TAG, "Invalid handle or channel");
    ESP_RETURN_ON_ERROR(gpio_set_level(handle->pins[channel], on), TAG, "Channel %u write failed", channel + 1);
    return ESP_OK;
}

esp_err_t mosaico_relay_all_off(mosaico_relay_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Missing handle");
    esp_err_t first_error = ESP_OK;
    for (unsigned i = 0; i < MOSAICO_RELAY_CHANNEL_COUNT; ++i) {
        esp_err_t ret = mosaico_relay_set(handle, i, false);
        if (first_error == ESP_OK) {
            first_error = ret;
        }
    }
    return first_error;
}

esp_err_t mosaico_relay_del(mosaico_relay_handle_t handle)
{
    ESP_RETURN_ON_ERROR(mosaico_relay_all_off(handle), TAG, "Switch-off failed; handle retained");
    /* Keep outputs low and leave the shared power rail enabled. */
    free(handle);
    return ESP_OK;
}
