/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file main.c
 * @brief Real-time ES8311 microphone-to-speaker loopback
 */

#include <stdint.h>
#include <string.h>
#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "audio_loopback";

#define LOOPBACK_SAMPLE_RATE      BSP_AUDIO_DEFAULT_SAMPLE_RATE
#define LOOPBACK_BITS_PER_SAMPLE  BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE
#define LOOPBACK_CHANNELS         BSP_AUDIO_DEFAULT_CHANNELS
#define LOOPBACK_IO_CHUNK_BYTES   (4 * 1024)

static esp_err_t codec_ret_to_esp_err(int ret, const char *operation)
{
    if (ret == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed (codec error=%d)", operation, ret);
    return ESP_FAIL;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ES8311 audio loopback: %d Hz, %d-bit, %d channel(s)",
             LOOPBACK_SAMPLE_RATE, LOOPBACK_BITS_PER_SAMPLE, LOOPBACK_CHANNELS);

    esp_codec_dev_handle_t microphone = bsp_audio_codec_microphone_init();
    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    if (!microphone || !speaker) {
        ESP_LOGE(TAG, "Failed to initialize ES8311 codec devices");
        return;
    }

    uint8_t *buffer = heap_caps_malloc(LOOPBACK_IO_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d-byte loopback buffer", LOOPBACK_IO_CHUNK_BYTES);
        return;
    }
    memset(buffer, 0, LOOPBACK_IO_CHUNK_BYTES);

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = LOOPBACK_SAMPLE_RATE,
        .bits_per_sample = LOOPBACK_BITS_PER_SAMPLE,
        .channel = LOOPBACK_CHANNELS,
        .channel_mask = 0,
    };

    ESP_ERROR_CHECK(codec_ret_to_esp_err(esp_codec_dev_open(microphone, &sample_info), "open microphone"));
    ESP_ERROR_CHECK(codec_ret_to_esp_err(esp_codec_dev_open(speaker, &sample_info), "open speaker"));
    ESP_ERROR_CHECK(codec_ret_to_esp_err(esp_codec_dev_set_in_gain(microphone, 30.0f), "set microphone gain"));
    ESP_ERROR_CHECK(codec_ret_to_esp_err(esp_codec_dev_set_out_vol(speaker, 50), "set speaker volume"));

    ESP_LOGI(TAG, "Audio loopback is running");
    while (true) {
        ESP_ERROR_CHECK(codec_ret_to_esp_err(
            esp_codec_dev_read(microphone, buffer, LOOPBACK_IO_CHUNK_BYTES), "read microphone"));
        ESP_ERROR_CHECK(codec_ret_to_esp_err(
            esp_codec_dev_write(speaker, buffer, LOOPBACK_IO_CHUNK_BYTES), "write speaker"));
    }
}
