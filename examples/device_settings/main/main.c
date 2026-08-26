/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include "app_settings.h"
#include "esp_log.h"

static const char *TAG = "device_settings";

void app_main(void)
{
    app_settings_t settings;
    ESP_ERROR_CHECK(app_settings_storage_init());
    ESP_ERROR_CHECK(app_settings_load(&settings));

    ESP_LOGI(TAG, "name=%s brightness=%u volume=%u report_interval=%" PRIu32 "s",
             settings.device_name,
             settings.display_brightness_percent,
             settings.speaker_volume_percent,
             settings.reporting_interval_s);
    ESP_LOGI(TAG, "Call app_settings_save() after a validated user change");
    ESP_LOGI(TAG, "Call app_settings_factory_reset() only from an explicit recovery action");
}
