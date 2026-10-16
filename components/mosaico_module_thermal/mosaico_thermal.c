/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdlib.h>
#include "mosaico_module_thermal.h"
#include "MLX90640_API.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mosaico_thermal";

struct mosaico_thermal {
    mlx90640_io_t io;
    mosaico_thermal_config_t config;
    paramsMLX90640 params;
    uint16_t raw[834];
    mosaico_thermal_frame_t frame;
};

static esp_err_t sensor_error(mosaico_thermal_handle_t sensor, const char *operation, int status)
{
    esp_err_t ret = sensor->io.error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : sensor->io.error;
    ESP_LOGW(TAG, "%s failed: status=%d error=%s", operation, status, esp_err_to_name(ret));
    return ret;
}

esp_err_t mosaico_thermal_new(bsp_subboard_slot_t slot, const mosaico_thermal_config_t *config, mosaico_thermal_handle_t *out_sensor)
{
    if (!out_sensor) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_sensor = NULL;
    const mosaico_thermal_config_t defaults = MOSAICO_THERMAL_DEFAULT_CONFIG();
    if (!config) {
        config = &defaults;
    }
    if (slot < BSP_SUBBOARD_SLOT_LEFT || slot >= BSP_SUBBOARD_SLOT_COUNT || !config->clock_speed_hz || config->clock_speed_hz > 1000000 || config->refresh_rate > 7 ||
        !isfinite(config->emissivity) || config->emissivity <= 0 || config->emissivity > 1 || !isfinite(config->reflected_offset_c)) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "Subboard bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = bsp_power_set_codec_3v3(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable module rail failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* The module 5V rail shares the codec power control. */
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    mosaico_thermal_handle_t sensor = calloc(1, sizeof(*sensor));
    if (!sensor) {
        return ESP_ERR_NO_MEM;
    }
    sensor->config = *config;
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x33,
        .scl_speed_hz = config->clock_speed_hz,
    };
    ret = i2c_master_bus_add_device(bus, &device, &sensor->io.device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Attach failed: %s", esp_err_to_name(ret));
        free(sensor);
        return ret;
    }
    sensor->io.deadline_us = esp_timer_get_time() + 2000000;
    int status = MLX90640_DumpEE(&sensor->io, sensor->raw);
    if (!status) {
        status = MLX90640_ExtractParameters(sensor->raw, &sensor->params);
    }
    if (!status && (!sensor->params.kVdd || !sensor->params.KtPTAT || !sensor->params.gainEE)) {
        status = -MLX90640_EEPROM_DATA_ERROR;
    }
    if (!status) {
        status = MLX90640_SetChessMode(&sensor->io);
    }
    if (!status) {
        status = MLX90640_SetRefreshRate(&sensor->io, config->refresh_rate);
    }
    if (!status) {
        status = MLX90640_SetResolution(&sensor->io, 1);
    }
    if (status) {
        ret = sensor_error(sensor, "Initialize", status);
        /* No transfers remain in flight on the synchronous bus. */
        esp_err_t cleanup = i2c_master_bus_rm_device(sensor->io.device);
        if (cleanup != ESP_OK) {
            ESP_LOGE(TAG, "Initialization cleanup failed: %s", esp_err_to_name(cleanup));
            *out_sensor = sensor;
            return cleanup;
        }
        free(sensor);
        return ret;
    }
    *out_sensor = sensor;
    return ESP_OK;
}

esp_err_t mosaico_thermal_read(mosaico_thermal_handle_t sensor, mosaico_thermal_frame_t *frame, int timeout_ms)
{
    if (!sensor || !frame || timeout_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor->io.error = ESP_OK;
    sensor->io.deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    unsigned pages = 0;
    while (pages != 3) {
        int status = MLX90640_GetFrameData(&sensor->io, sensor->raw);
        if (status == -MLX90640_FRAME_OVERRUN_ERROR) {
            /* Retry within the same deadline; never combine pages across an overrun. */
            pages = 0;
            continue;
        }
        if (status < 0) {
            return sensor_error(sensor, "Read frame", status);
        }
        sensor->frame.ambient_c = MLX90640_GetTa(sensor->raw, &sensor->params);
        sensor->frame.supply_v = MLX90640_GetVdd(sensor->raw, &sensor->params);
        float reflected = sensor->frame.ambient_c - sensor->config.reflected_offset_c;
        if (!isfinite(sensor->frame.ambient_c) || !isfinite(sensor->frame.supply_v) || reflected <= -273.15f || !sensor->raw[778]) {
            return sensor_error(sensor, "Invalid auxiliary data", status);
        }
        MLX90640_CalculateTo(sensor->raw, &sensor->params, sensor->config.emissivity, reflected, sensor->frame.pixels);
        pages |= 1U << status;
    }
    /* Correction may depend on neighboring pixels from the opposite subpage. */
    MLX90640_BadPixelsCorrection(sensor->params.brokenPixels, sensor->frame.pixels, 1, &sensor->params);
    MLX90640_BadPixelsCorrection(sensor->params.outlierPixels, sensor->frame.pixels, 1, &sensor->params);
    for (unsigned i = 0; i < MOSAICO_THERMAL_PIXEL_COUNT; ++i) {
        if (!isfinite(sensor->frame.pixels[i])) {
            return sensor_error(sensor, "Invalid temperature", 0);
        }
    }
    *frame = sensor->frame;
    return ESP_OK;
}

esp_err_t mosaico_thermal_del(mosaico_thermal_handle_t sensor)
{
    if (!sensor) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_master_bus_rm_device(sensor->io.device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Detach failed: %s", esp_err_to_name(ret));
        return ret;
    }
    free(sensor);
    return ESP_OK;
}
