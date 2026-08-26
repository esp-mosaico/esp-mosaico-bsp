/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "S31-Mosaico-Audio";
static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static const audio_codec_data_if_t *s_data_if;
static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_microphone;

static i2s_std_config_t default_i2s_config(void)
{
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK,
            .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK,
            .dout = BSP_AUDIO_I2S_SDOUT,
            .din = BSP_AUDIO_I2S_DSIN,
        },
    };
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    return config;
}

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    if (s_data_if) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 rail failed");
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG, "enable codec 3V3 power failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx_channel, &s_rx_channel), TAG,
                        "create I2S channels failed");
    i2s_std_config_t fallback = default_i2s_config();
    const i2s_std_config_t *active = i2s_config ? i2s_config : &fallback;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_channel, active), TAG, "initialize I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, active), TAG, "initialize I2S RX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_channel), TAG, "enable I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_channel), TAG, "enable I2S RX failed");
    audio_codec_i2s_cfg_t data_config = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx_channel,
        .rx_handle = s_rx_channel,
    };
    s_data_if = audio_codec_new_i2s_data(&data_config);
    ESP_RETURN_ON_FALSE(s_data_if, ESP_FAIL, TAG, "create codec I2S data interface failed");
    ESP_LOGI(TAG, "I2S initialized: MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d",
             active->gpio_cfg.mclk, active->gpio_cfg.bclk,
             active->gpio_cfg.ws, active->gpio_cfg.dout,
             active->gpio_cfg.din);
    return ESP_OK;
}

static esp_codec_dev_handle_t create_codec(esp_codec_dev_type_t type, int16_t pa_pin)
{
    if (bsp_i2c_init() != ESP_OK || bsp_audio_init(NULL) != ESP_OK) {
        return NULL;
    }
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) {
        ESP_LOGE(TAG, "create codec GPIO interface failed");
        return NULL;
    }
    audio_codec_i2c_cfg_t i2c_config = {
        .addr = BSP_AUDIO_CODEC_I2C_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
        .clock_speed_hz = 0,
    };
    const audio_codec_ctrl_if_t *control_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!control_if) {
        ESP_LOGE(TAG, "create ES8311 I2C control interface failed");
        return NULL;
    }
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control_if,
        .gpio_if = gpio_if,
        .sys_cfg = {
            .is_master = false,
            .no_mclk = false,
        },
        .adc_cfg = {
            .digital_mic = false,
            .label = NULL,
        },
        .dac_cfg = {
            .ref_enable = false,
        },
        .pa_cfg = {
            .pa_pin = pa_pin,
            .pa_active_low = false,
            .hw_gain = {
                .pa_voltage = 5.0f,
                .codec_dac_voltage = 3.3f,
            },
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_config);
    if (!codec_if) {
        ESP_LOGE(TAG, "create ES8311 codec interface failed");
        return NULL;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = type,
        .codec_if = codec_if,
        .data_if = s_data_if,
    };
    return esp_codec_dev_new(&device_config);
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (!s_speaker) {
        s_speaker = create_codec(ESP_CODEC_DEV_TYPE_OUT, BSP_AUDIO_PA_CTRL);
    }
    return s_speaker;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (!s_microphone) {
        s_microphone = create_codec(ESP_CODEC_DEV_TYPE_IN, GPIO_NUM_NC);
    }
    return s_microphone;
}
