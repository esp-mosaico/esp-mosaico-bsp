/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief WakeNet9 + MultiNet7 + Xiaole TTS on ESP-Mosaico (esp-sr).
 */

#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_partition.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

static const char *TAG = "voice_wake_cmd";

static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static volatile bool s_running = true;
static esp_tts_handle_t s_tts;
static esp_tts_voice_t *s_tts_voice;
static esp_partition_mmap_handle_t s_voice_mmap;
static esp_codec_dev_handle_t s_spk;
static SemaphoreHandle_t s_speak_lock;
static srmodel_list_t *s_models;

static esp_err_t tts_init(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG,
                        "voice_data partition not found");

    const void *voice_data = NULL;
    ESP_RETURN_ON_ERROR(
        esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA,
                           &voice_data, &s_voice_mmap),
        TAG, "map voice_data");

    s_tts_voice = esp_tts_voice_set_init(&esp_tts_voice_template, (void *)voice_data);
    if (!s_tts_voice) {
        esp_partition_munmap(s_voice_mmap);
        s_voice_mmap = 0;
        ESP_LOGE(TAG, "initialize Xiaole voice from voice_data failed");
        return ESP_FAIL;
    }

    s_tts = esp_tts_create(s_tts_voice);
    if (!s_tts) {
        esp_tts_voice_set_free(s_tts_voice);
        s_tts_voice = NULL;
        esp_partition_munmap(s_voice_mmap);
        s_voice_mmap = 0;
        ESP_LOGE(TAG, "esp_tts_create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTS voice initialized from voice_data (%lu bytes)",
             (unsigned long)partition->size);
    return ESP_OK;
}

static void say(const char *text)
{
    if (!s_tts || !s_spk || !text) {
        return;
    }
    if (xSemaphoreTake(s_speak_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return;
    }

    ESP_LOGI(TAG, "TTS: %s", text);
    if (esp_tts_parse_chinese(s_tts, text)) {
        int len = 0;
        while (true) {
            short *pcm = esp_tts_stream_play(s_tts, &len, 3);
            if (len <= 0) {
                break;
            }
            (void)esp_codec_dev_write(s_spk, pcm, len * (int)sizeof(short));
        }
    }
    esp_tts_stream_reset(s_tts);
    xSemaphoreGive(s_speak_lock);
}

static void feed_task(void *arg)
{
    (void)arg;

    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (!mic) {
        ESP_LOGE(TAG, "microphone init failed");
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t in = {
        .sample_rate = 16000,
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
    };
    if (esp_codec_dev_open(mic, &in) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphone open failed");
        vTaskDelete(NULL);
        return;
    }
    (void)esp_codec_dev_set_in_gain(mic, 30.0f);

    int chunksize = s_afe_handle->get_feed_chunksize(s_afe_data);
    int nch = s_afe_handle->get_feed_channel_num(s_afe_data);
    size_t bytes = (size_t)chunksize * sizeof(int16_t) * (size_t)nch;
    int16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "feed buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        if (esp_codec_dev_read(mic, buf, (int)bytes) != ESP_CODEC_DEV_OK) {
            continue;
        }
        s_afe_handle->feed(s_afe_data, buf);
    }

    free(buf);
    vTaskDelete(NULL);
}

static void detect_task(void *arg)
{
    (void)arg;

    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "MultiNet model not found in model partition");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "multinet: %s", mn_name);

    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *mn_data = multinet->create(mn_name, 6000);
    esp_mn_commands_clear();
    esp_mn_commands_add(1, "da kai deng");
    esp_mn_commands_add(2, "guan bi deng");
    esp_mn_commands_update();

    int wakeup = 0;
    while (s_running) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || res->ret_value != ESP_OK) {
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected");
            say("我在");
            multinet->clean(mn_data);
            wakeup = 1;
        }

        if (!wakeup) {
            continue;
        }

        esp_mn_state_t st = multinet->detect(mn_data, res->data);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = multinet->get_results(mn_data);
            if (r && r->num > 0) {
                ESP_LOGI(TAG, "command id=%d: %s", r->command_id[0], r->string);
                if (r->command_id[0] == 1) {
                    say("好的，开灯");
                } else if (r->command_id[0] == 2) {
                    say("好的，关灯");
                } else {
                    say("收到");
                }
            }
            wakeup = 0;
            s_afe_handle->enable_wakenet(s_afe_data);
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "command timeout, listening for wake word");
            wakeup = 0;
            s_afe_handle->enable_wakenet(s_afe_data);
        }
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    s_speak_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_speak_lock ? ESP_OK : ESP_ERR_NO_MEM);
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker init failed");
        return;
    }
    esp_codec_dev_sample_info_t out = {
        .sample_rate = 16000,
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
    };
    if (esp_codec_dev_open(s_spk, &out) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        return;
    }
    (void)esp_codec_dev_set_out_vol(s_spk, 60);
    ESP_ERROR_CHECK(tts_init());

    s_models = esp_srmodel_init("model");
    ESP_ERROR_CHECK(s_models ? ESP_OK : ESP_ERR_NOT_FOUND);
    char *wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "hilexin");
    if (!wn_name) {
        ESP_LOGE(TAG, "WakeNet model not found; flash the model partition");
        return;
    }
    ESP_LOGI(TAG, "wakenet: %s", wn_name);

    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_ERROR_CHECK(cfg ? ESP_OK : ESP_ERR_NO_MEM);
    cfg->aec_init = false;
    cfg->se_init = false;
    cfg->ns_init = false;
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_3;
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = wn_name;
    cfg->wakenet_mode = DET_MODE_95;
    cfg->agc_init = true;

    s_afe_handle = esp_afe_handle_from_config(cfg);
    if (!s_afe_handle) {
        afe_config_free(cfg);
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        return;
    }
    s_afe_data = s_afe_handle->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return;
    }

    say("你好，欢迎使用语音交互");

    xTaskCreatePinnedToCore(feed_task, "feed", 6 * 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "detect", 8 * 1024, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "Listening for wake word (Hi,乐鑫) then commands");
}
