/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief Play embedded MP3 via esp_audio_simple_player to BSP ES8311.
 *
 * alarm.mp3 is copied from esp-gmf packages/esp_audio_simple_player test_apps
 * (Espressif Apache-2.0 sample tone).
 */

#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_codec_dev.h"
#include "esp_gmf_io_embed_flash.h"
#include "esp_gmf_pipeline.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "gmf_player";

extern const uint8_t alarm_mp3_start[] asm("_binary_alarm_mp3_start");
extern const uint8_t alarm_mp3_end[] asm("_binary_alarm_mp3_end");

static const char *EMBED_URI = "embed://tone/0_alarm.mp3";

static esp_codec_dev_handle_t s_speaker;
static SemaphoreHandle_t s_done;

static int out_data_callback(uint8_t *data, int data_size, void *ctx)
{
    esp_codec_dev_handle_t dev = (esp_codec_dev_handle_t)ctx;
    if (dev && data && data_size > 0) {
        (void)esp_codec_dev_write(dev, data, data_size);
    }
    return 0;
}

static int event_callback(esp_asp_event_pkt_t *event, void *ctx)
{
    (void)ctx;
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO) {
        esp_asp_music_info_t info = {0};
        memcpy(&info, event->payload, event->payload_size);
        ESP_LOGI(TAG, "music info: rate=%d ch=%d bits=%d",
                 info.sample_rate, info.channels, info.bits);

        if (s_speaker) {
            (void)esp_codec_dev_close(s_speaker);
            esp_codec_dev_sample_info_t sample_info = {
                .sample_rate = info.sample_rate,
                .bits_per_sample = info.bits,
                .channel = info.channels,
                .channel_mask = 0,
            };
            if (esp_codec_dev_open(s_speaker, &sample_info) != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "reopen speaker failed");
            } else {
                (void)esp_codec_dev_set_out_vol(s_speaker, 60);
            }
        }
    } else if (event->type == ESP_ASP_EVENT_TYPE_STATE) {
        esp_asp_state_t st = 0;
        memcpy(&st, event->payload, event->payload_size);
        ESP_LOGI(TAG, "state=%s", esp_audio_simple_player_state_to_str(st));
        if (s_done &&
            (st == ESP_ASP_STATE_STOPPED || st == ESP_ASP_STATE_FINISHED ||
             st == ESP_ASP_STATE_ERROR)) {
            xSemaphoreGive(s_done);
        }
    }
    return 0;
}

static int embed_flash_io_set(esp_asp_handle_t handle, void *ctx)
{
    (void)ctx;
    esp_gmf_pipeline_handle_t pipe = NULL;
    int ret = esp_audio_simple_player_get_pipeline(handle, &pipe);
    if (ret != ESP_GMF_ERR_OK || pipe == NULL) {
        return ret;
    }

    esp_gmf_io_handle_t flash = NULL;
    ret = esp_gmf_pipeline_get_in(pipe, &flash);
    if (ret != ESP_GMF_ERR_OK || flash == NULL) {
        return ret;
    }

    const embed_item_info_t item = {
        .address = alarm_mp3_start,
        .size = (int)(alarm_mp3_end - alarm_mp3_start),
    };
    return esp_gmf_io_embed_flash_set_context(flash, &item, 1);
}

void app_main(void)
{
    ESP_LOGI(TAG, "GMF simple player → ES8311");

    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) {
        ESP_LOGE(TAG, "speaker init failed");
        return;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = BSP_AUDIO_DEFAULT_SAMPLE_RATE,
        .bits_per_sample = BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
        .channel = BSP_AUDIO_DEFAULT_CHANNELS,
        .channel_mask = 0,
    };
    if (esp_codec_dev_open(s_speaker, &sample_info) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        return;
    }
    (void)esp_codec_dev_set_out_vol(s_speaker, 60);

    s_done = xSemaphoreCreateBinary();
    if (!s_done) {
        ESP_LOGE(TAG, "semaphore alloc failed");
        return;
    }

    esp_asp_cfg_t cfg = {
        .in.cb = NULL,
        .in.user_ctx = NULL,
        .out.cb = out_data_callback,
        .out.user_ctx = s_speaker,
        .task_prio = 5,
        .task_stack = 4096,
        .task_core = 0,
        .task_stack_in_ext = true,
        .prev = embed_flash_io_set,
        .prev_ctx = NULL,
    };

    esp_asp_handle_t player = NULL;
    if (esp_audio_simple_player_new(&cfg, &player) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "player new failed");
        return;
    }
    if (esp_audio_simple_player_set_event(player, event_callback, NULL) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "set event failed");
        return;
    }

    while (true) {
        ESP_LOGI(TAG, "play %s (%d bytes)", EMBED_URI,
                 (int)(alarm_mp3_end - alarm_mp3_start));
        (void)xSemaphoreTake(s_done, 0);
        if (esp_audio_simple_player_run(player, EMBED_URI, NULL) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "run failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGW(TAG, "play timeout, stopping");
            (void)esp_audio_simple_player_stop(player);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
