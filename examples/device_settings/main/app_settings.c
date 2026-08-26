/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "app_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_NAMESPACE "appcfg"
#define SETTINGS_SCHEMA_VERSION 2

static const char *TAG = "app_settings";

static void set_defaults(app_settings_t *settings)
{
    *settings = (app_settings_t) {
        .display_brightness_percent = 70,
        .speaker_volume_percent = 60,
        .reporting_interval_s = 60,
        .device_name = "mosaico",
    };
}

static bool validate(app_settings_t *settings)
{
    bool changed = false;
    if (settings->display_brightness_percent > 100) {
        settings->display_brightness_percent = 70;
        changed = true;
    }
    if (settings->speaker_volume_percent > 100) {
        settings->speaker_volume_percent = 60;
        changed = true;
    }
    if (settings->reporting_interval_s < 5 || settings->reporting_interval_s > 86400) {
        settings->reporting_interval_s = 60;
        changed = true;
    }
    settings->device_name[APP_SETTINGS_DEVICE_NAME_MAX - 1] = '\0';
    if (settings->device_name[0] == '\0') {
        strlcpy(settings->device_name, "mosaico", sizeof(settings->device_name));
        changed = true;
    }
    return changed;
}

esp_err_t app_settings_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires recovery: %s", esp_err_to_name(ret));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "initialize NVS failed");
    ESP_LOGI(TAG, "Settings storage initialized");
    return ESP_OK;
}

esp_err_t app_settings_save(const app_settings_t *settings)
{
    ESP_RETURN_ON_FALSE(settings, ESP_ERR_INVALID_ARG, TAG, "settings is null");
    app_settings_t checked = *settings;
    ESP_RETURN_ON_FALSE(!validate(&checked), ESP_ERR_INVALID_ARG, TAG, "settings are invalid");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "open settings namespace failed");
    esp_err_t ret = nvs_set_u16(handle, "schema", SETTINGS_SCHEMA_VERSION);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, "brightness", checked.display_brightness_percent);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, "volume", checked.speaker_volume_percent);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u32(handle, "report_s", checked.reporting_interval_s);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, "name", checked.device_name);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Save settings failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Settings saved with schema=%u", SETTINGS_SCHEMA_VERSION);
    return ESP_OK;
}

esp_err_t app_settings_load(app_settings_t *settings)
{
    ESP_RETURN_ON_FALSE(settings, ESP_ERR_INVALID_ARG, TAG, "settings is null");
    set_defaults(settings);

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "open settings namespace failed");
    uint16_t schema = 0;
    esp_err_t ret = nvs_get_u16(handle, "schema", &schema);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        ESP_LOGI(TAG, "No stored settings; writing defaults");
        return app_settings_save(settings);
    }
    if (ret != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Read settings schema failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (schema > SETTINGS_SCHEMA_VERSION) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Unsupported settings schema=%u", schema);
        return ESP_ERR_INVALID_VERSION;
    }

    bool dirty = schema < SETTINGS_SCHEMA_VERSION;
    if (nvs_get_u8(handle, "brightness", &settings->display_brightness_percent) != ESP_OK) {
        dirty = true;
    }
    if (nvs_get_u8(handle, "volume", &settings->speaker_volume_percent) != ESP_OK) {
        dirty = true;
    }
    if (nvs_get_u32(handle, "report_s", &settings->reporting_interval_s) != ESP_OK) {
        dirty = true;
    }
    size_t name_size = sizeof(settings->device_name);
    if (nvs_get_str(handle, "name", settings->device_name, &name_size) != ESP_OK) {
        dirty = true;
    }
    nvs_close(handle);

    dirty |= validate(settings);
    if (dirty) {
        ESP_LOGI(TAG, "Migrating or repairing settings schema=%u -> %u",
                 schema, SETTINGS_SCHEMA_VERSION);
        return app_settings_save(settings);
    }
    ESP_LOGI(TAG, "Settings loaded with schema=%u", schema);
    return ESP_OK;
}

esp_err_t app_settings_factory_reset(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "open settings namespace failed");
    esp_err_t ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Application settings namespace erased");
    return ESP_OK;
}
