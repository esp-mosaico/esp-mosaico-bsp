/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief Chinese TTS via esp-sr Xiaole voice on ESP-Mosaico.
 */

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice_tts";

static esp_partition_mmap_handle_t s_voice_mmap;
static esp_tts_voice_t *s_voice;

static esp_err_t tts_init(esp_tts_handle_t *out_tts)
{
    ESP_RETURN_ON_FALSE(out_tts, ESP_ERR_INVALID_ARG, TAG, "TTS output is null");

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG,
                        "voice_data partition not found");

    const void *voice_data = NULL;
    ESP_RETURN_ON_ERROR(
        esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA,
                           &voice_data, &s_voice_mmap),
        TAG, "map voice_data");

    s_voice = esp_tts_voice_set_init(&esp_tts_voice_template, (void *)voice_data);
    if (!s_voice) {
        esp_partition_munmap(s_voice_mmap);
        s_voice_mmap = 0;
        ESP_LOGE(TAG, "initialize Xiaole voice from voice_data failed");
        return ESP_FAIL;
    }

    *out_tts = esp_tts_create(s_voice);
    if (!*out_tts) {
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
        esp_partition_munmap(s_voice_mmap);
        s_voice_mmap = 0;
        ESP_LOGE(TAG, "esp_tts_create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTS voice initialized from voice_data (%lu bytes)",
             (unsigned long)partition->size);
    return ESP_OK;
}

static void say(esp_tts_handle_t tts, esp_codec_dev_handle_t spk, const char *text)
{
    ESP_LOGI(TAG, "say: %s", text);
    if (!esp_tts_parse_chinese(tts, text)) {
        ESP_LOGW(TAG, "parse failed");
        return;
    }

    int len = 0;
    while (true) {
        short *pcm = esp_tts_stream_play(tts, &len, 3);
        if (len <= 0) {
            break;
        }
        (void)esp_codec_dev_write(spk, pcm, len * (int)sizeof(short));
    }
    esp_tts_stream_reset(tts);
}

void app_main(void)
{
    esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
    if (!spk) {
        ESP_LOGE(TAG, "speaker init failed");
        return;
    }

    esp_codec_dev_sample_info_t out = {
        .sample_rate = 16000,
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
    };
    if (esp_codec_dev_open(spk, &out) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        return;
    }
    (void)esp_codec_dev_set_out_vol(spk, 60);

    esp_tts_handle_t tts = NULL;
    ESP_ERROR_CHECK(tts_init(&tts));

    static const char *phrases[] = {
        "你好，欢迎使用莫塞克语音合成",
        "现在可以播报中文提示音",
        "测试完成",
    };

    while (true) {
        for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
            say(tts, spk, phrases[i]);
            vTaskDelay(pdMS_TO_TICKS(800));
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
