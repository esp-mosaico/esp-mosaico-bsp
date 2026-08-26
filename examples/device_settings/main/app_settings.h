/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SETTINGS_DEVICE_NAME_MAX 32

typedef struct {
    uint8_t display_brightness_percent;
    uint8_t speaker_volume_percent;
    uint32_t reporting_interval_s;
    char device_name[APP_SETTINGS_DEVICE_NAME_MAX];
} app_settings_t;

esp_err_t app_settings_storage_init(void);
esp_err_t app_settings_load(app_settings_t *settings);
esp_err_t app_settings_save(const app_settings_t *settings);
esp_err_t app_settings_factory_reset(void);

#ifdef __cplusplus
}
#endif
