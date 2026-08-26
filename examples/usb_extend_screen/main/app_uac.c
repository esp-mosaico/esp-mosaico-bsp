/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_usb.h"
#include "bsp/audio.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "usb_descriptors.h"
#include "usb_device_uac.h"

static const char *TAG = "app_uac";

static esp_codec_dev_handle_t s_playback_codec;
static esp_codec_dev_handle_t s_record_codec;

static esp_err_t uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    ESP_RETURN_ON_FALSE(s_playback_codec, ESP_ERR_INVALID_STATE, TAG, "playback codec is unavailable");
    return esp_codec_dev_write(s_playback_codec, buf, len) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t uac_device_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    ESP_RETURN_ON_FALSE(s_record_codec, ESP_ERR_INVALID_STATE, TAG, "record codec is unavailable");
    int ret = esp_codec_dev_read(s_record_codec, buf, len);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    *bytes_read = len;
    return ESP_OK;
}

static void uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    if (s_playback_codec) {
        esp_codec_dev_set_out_mute(s_playback_codec, mute);
    }
}

static void uac_device_set_volume_cb(uint32_t volume, void *arg)
{
    if (s_playback_codec) {
        esp_codec_dev_set_out_vol(s_playback_codec, volume);
    }
}

static esp_err_t app_uac_open_codec(esp_codec_dev_handle_t codec, uint8_t channels)
{
    ESP_RETURN_ON_FALSE(codec, ESP_ERR_INVALID_STATE, TAG, "codec handle is NULL");

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = CONFIG_UAC_SAMPLE_RATE,
        .channel = channels,
        .bits_per_sample = CONFIG_UAC_BYTES_PER_SAMPLE * 8,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(codec, &sample_info) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec failed");
    return ESP_OK;
}

esp_err_t app_uac_init(void)
{
#if CONFIG_UAC_SPEAKER_CHANNEL_NUM > 0
    s_playback_codec = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_ERROR(app_uac_open_codec(s_playback_codec, CONFIG_UAC_SPEAKER_CHANNEL_NUM),
                        TAG, "initialize playback codec failed");
    esp_codec_dev_set_out_vol(s_playback_codec, 60);
#endif
#if CONFIG_UAC_MIC_CHANNEL_NUM > 0
    s_record_codec = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_ERROR(app_uac_open_codec(s_record_codec, CONFIG_UAC_MIC_CHANNEL_NUM),
                        TAG, "initialize record codec failed");
    int ret = esp_codec_dev_set_in_gain(s_record_codec, CONFIG_UAC_MIC_GAIN_DB);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to set microphone gain to %d dB: %d",
                 CONFIG_UAC_MIC_GAIN_DB, ret);
    }
#endif

    uac_device_config_t config = {
        .skip_tinyusb_init = true,
        .output_cb = uac_device_output_cb,
        .input_cb = uac_device_input_cb,
        .set_mute_cb = uac_device_set_mute_cb,
        .set_volume_cb = uac_device_set_volume_cb,
        .cb_ctx = NULL,
#if CONFIG_UAC_SPEAKER_CHANNEL_NUM > 0
        .spk_itf_num = ITF_NUM_AUDIO_STREAMING_SPK,
#endif
#if CONFIG_UAC_MIC_CHANNEL_NUM > 0
        .mic_itf_num = ITF_NUM_AUDIO_STREAMING_MIC,
#endif
    };

    return uac_device_init(&config);
}
