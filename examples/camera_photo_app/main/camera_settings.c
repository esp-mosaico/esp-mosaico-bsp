/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_settings.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "camera_settings";

#define NVS_NAMESPACE       "camera_app"
#define NVS_KEY_PREVIEW_FLIP "prev_flip"

esp_err_t camera_settings_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "initialize NVS failed");
    return ESP_OK;
}

esp_err_t camera_settings_load_preview_flip(bool *flipped)
{
    ESP_RETURN_ON_FALSE(flipped, ESP_ERR_INVALID_ARG, TAG, "output pointer is null");

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *flipped = false;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open NVS namespace failed");

    uint8_t value = 0;
    ret = nvs_get_u8(handle, NVS_KEY_PREVIEW_FLIP, &value);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *flipped = false;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "read preview flip setting failed");

    *flipped = value != 0;
    return ESP_OK;
}

esp_err_t camera_settings_save_preview_flip(bool flipped)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(
        nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle),
        TAG, "open NVS namespace failed");
    ESP_RETURN_ON_ERROR(
        nvs_set_u8(handle, NVS_KEY_PREVIEW_FLIP, flipped ? 1U : 0U),
        TAG, "write preview flip setting failed");
    ESP_RETURN_ON_ERROR(nvs_commit(handle), TAG, "commit preview flip setting failed");
    nvs_close(handle);
    return ESP_OK;
}
