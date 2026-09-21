/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a1_audio.h"
#include "headphone_debounce.h"

#include <inttypes.h>
#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "bsp/esp_mosaico.h"
#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "esp32_a1_codec.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define A1_POWER_GPIO             GPIO_NUM_10
#define A1_PA_GPIO                GPIO_NUM_38
#define A1_AUDIO_DETECT_GPIO      GPIO_NUM_47
#define A1_I2S_MCLK_GPIO          GPIO_NUM_54
#define A1_I2S_BCLK_GPIO          GPIO_NUM_37
#define A1_I2S_DOUT_GPIO          GPIO_NUM_52
#define A1_I2S_WS_GPIO            GPIO_NUM_49
#define A1_I2S_DIN_GPIO           GPIO_NUM_40
#define A1_I2C_ADDRESS_7BIT       0x2c
#define A1_I2C_ADDRESS_8BIT       (A1_I2C_ADDRESS_7BIT << 1)
#define A1_I2S_SLOT_COUNT         2
#define A1_MCLK_MULTIPLE          384
#define A1_DETECT_RETRIES         20
#define A1_DETECT_RETRY_MS        50
#define A1_HP_INSERTED_LEVEL      0

static const char *TAG = "a1_audio";
static i2s_chan_handle_t s_tx_channel;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_dac;
static bool s_open;
static bool s_muted = true;
static portMUX_TYPE s_route_lock = portMUX_INITIALIZER_UNLOCKED;
static headphone_debounce_t s_hp_state;
static bool s_hp_task_started;
static uint32_t s_sample_rate;

static esp_err_t codec_result(int result, const char *operation)
{
    if (result == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed: %d", operation, result);
    return ESP_FAIL;
}

/* Caller holds s_route_lock. Never read the raw detect pin here: mute/unmute
 * and sample-rate changes must not bypass the detection state machine. */
static void apply_output_route_locked(void)
{
    gpio_set_level(A1_PA_GPIO, !s_muted && s_hp_state.speaker_allowed);
}

static void hp_detect_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "headphone debounce: sample=10 ms, insert=%u ms, remove=%u ms",
             HP_INSERT_STABLE_MS, HP_REMOVE_STABLE_MS);
    while (true) {
        bool inserted=gpio_get_level(A1_AUDIO_DETECT_GPIO)==A1_HP_INSERTED_LEVEL;
        uint32_t now_ms=(uint32_t)(esp_timer_get_time()/1000);
        portENTER_CRITICAL(&s_route_lock);
        bool changed=headphone_debounce_update(&s_hp_state,inserted,now_ms);
        apply_output_route_locked();
        bool confirmed=s_hp_state.confirmed;
        portEXIT_CRITICAL(&s_route_lock);
        if(changed)ESP_LOGI(TAG,"audio route (debounced): %s",confirmed?"headphones":"speaker");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t hold_i2s_straps_low(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(A1_I2S_MCLK_GPIO) | BIT64(A1_I2S_BCLK_GPIO) |
                        BIT64(A1_I2S_WS_GPIO) | BIT64(A1_I2S_DOUT_GPIO) |
                        BIT64(A1_I2S_DIN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure A1 I2S strap pins failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_MCLK_GPIO, 0), TAG, "hold MCLK low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_BCLK_GPIO, 0), TAG, "hold BCLK low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_WS_GPIO, 0), TAG, "hold WS low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_DOUT_GPIO, 0), TAG, "hold DOUT low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_DIN_GPIO, 0), TAG, "hold DIN low failed");
    return ESP_OK;
}

static void release_i2s_pins(void)
{
    gpio_reset_pin(A1_I2S_MCLK_GPIO);
    gpio_reset_pin(A1_I2S_BCLK_GPIO);
    gpio_reset_pin(A1_I2S_WS_GPIO);
    gpio_reset_pin(A1_I2S_DOUT_GPIO);
    gpio_reset_pin(A1_I2S_DIN_GPIO);
}

static esp_err_t power_and_probe(i2c_master_bus_handle_t bus)
{
    const gpio_config_t power_config = {
        .pin_bit_mask = BIT64(A1_POWER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power_config), TAG, "configure CODEC_PWR_EN failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_POWER_GPIO, 0), TAG, "hold A1 power low failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_POWER_GPIO, 1), TAG, "enable A1 codec power failed");

    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (unsigned retry = 0; retry < A1_DETECT_RETRIES; ++retry) {
        vTaskDelay(pdMS_TO_TICKS(A1_DETECT_RETRY_MS));
        result = i2c_master_probe(bus, A1_I2C_ADDRESS_7BIT, 100);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "A1 audio subboard detected on external I2C at 0x%02x",
                     A1_I2C_ADDRESS_7BIT);
            return ESP_OK;
        }
    }
    gpio_set_level(A1_POWER_GPIO, 0);
    ESP_LOGW(TAG, "A1 audio subboard not detected at I2C address 0x%02x",
             A1_I2C_ADDRESS_7BIT);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t init_route_gpios(void)
{
    const gpio_config_t pa_config = {
        .pin_bit_mask = BIT64(A1_PA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa_config), TAG, "configure PA_CTRL failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_PA_GPIO, 0), TAG, "disable speaker PA failed");

    const gpio_config_t detect_config = {
        .pin_bit_mask = BIT64(A1_AUDIO_DETECT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&detect_config);
}

static esp_err_t init_i2s(void)
{
    release_i2s_pins();
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx_channel, NULL), TAG,
                        "create A1 I2S TX channel failed");

    i2s_tdm_config_t config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1),
        .gpio_cfg = {
            .mclk = A1_I2S_MCLK_GPIO,
            .bclk = A1_I2S_BCLK_GPIO,
            .ws = A1_I2S_WS_GPIO,
            .dout = A1_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    config.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    config.clk_cfg.mclk_multiple = A1_MCLK_MULTIPLE;
    config.slot_cfg.total_slot = A1_I2S_SLOT_COUNT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_tx_channel, &config), TAG,
                        "initialize A1 TDM output failed");
    return ESP_OK;
}

static esp_err_t init_codec(i2c_master_bus_handle_t bus)
{
    audio_codec_i2c_cfg_t i2c_config = {
        .addr = A1_I2C_ADDRESS_8BIT,
        .bus_handle = bus,
        .clock_speed_hz = 400000,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    ESP_RETURN_ON_FALSE(s_ctrl_if, ESP_FAIL, TAG, "create A1 I2C control failed");

    audio_codec_i2s_cfg_t i2s_config = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx_channel,
        .clk_src = I2S_CLK_SRC_APLL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_config);
    ESP_RETURN_ON_FALSE(s_data_if, ESP_FAIL, TAG, "create A1 I2S data interface failed");
    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if, ESP_FAIL, TAG, "create codec GPIO interface failed");

    esp32_a1_codec_cfg_t codec_config = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .sys_cfg = {
            .no_mclk = true,
        },
        .dac_cfg = {
            .ref_enable = false,
            .headphone = AUDIO_HW_HEADPHONE_ENABLE,
        },
        .pa_cfg = {
            .pa_pin = -1, /* App debounce logic is the sole owner of PA_CTRL. */
            .pa_active_low = false,
            .hw_gain = {
                .pa_voltage = 3.3f,
                .codec_dac_voltage = 3.3f,
                .pa_gain = 0.0f,
            },
        },
        .int_cfg = {
            /* GPIO11 is wired, but playback does not require the A1 IRQ service. */
            .int_pin = -1,
        },
    };
    s_codec_if = esp32_a1_codec_new(&codec_config);
    ESP_RETURN_ON_FALSE(s_codec_if, ESP_FAIL, TAG, "create A1 codec interface failed");

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_dac = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(s_dac, ESP_FAIL, TAG, "create A1 DAC device failed");
    return ESP_OK;
}

esp_err_t a1_audio_init(void)
{
    ESP_RETURN_ON_FALSE(!s_dac, ESP_ERR_INVALID_STATE, TAG, "A1 audio already initialized");
    ESP_RETURN_ON_ERROR(hold_i2s_straps_low(), TAG, "hold A1 boot straps failed");
    ESP_RETURN_ON_ERROR(bsp_subboard_init(), TAG, "initialize external subboard bus failed");
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "external I2C bus is null");
    ESP_RETURN_ON_ERROR(power_and_probe(bus), TAG, "A1 subboard probe failed");
    ESP_RETURN_ON_ERROR(init_route_gpios(), TAG, "initialize A1 audio route failed");
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "initialize A1 I2S failed");
    ESP_RETURN_ON_ERROR(init_codec(bus), TAG, "initialize A1 codec failed");
    ESP_LOGI(TAG, "A1 ready: TDM TX GPIO52, MCLK GPIO54, BCLK GPIO37, WS GPIO49, 384fs");
    return ESP_OK;
}

esp_err_t a1_audio_open(uint32_t sample_rate, uint8_t volume)
{
    ESP_RETURN_ON_FALSE(s_dac, ESP_ERR_INVALID_STATE, TAG, "A1 audio is not initialized");
    if (s_open && s_sample_rate == sample_rate) {
        return a1_audio_set_volume(volume);
    }
    if (s_open) {
        ESP_RETURN_ON_ERROR(a1_audio_set_mute(true), TAG, "mute before sample-rate change failed");
        ESP_RETURN_ON_ERROR(codec_result(esp_codec_dev_close(s_dac), "close A1 DAC"), TAG,
                            "close A1 DAC before rate change failed");
        s_open = false;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate,
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .mclk_multiple = A1_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_ERROR(codec_result(esp_codec_dev_open(s_dac, &sample_info), "open A1 DAC"),
                        TAG, "open A1 DAC failed");
    s_open = true;
    s_sample_rate = sample_rate;
    ESP_RETURN_ON_ERROR(a1_audio_set_volume(volume), TAG, "set A1 volume failed");
    ESP_RETURN_ON_ERROR(a1_audio_set_mute(true), TAG, "mute A1 before PCM failed");

    if (!s_hp_task_started) {
        ESP_RETURN_ON_FALSE(xTaskCreate(hp_detect_task, "a1_hp_det", 2048, NULL, 3, NULL) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "create headphone detect task failed");
        s_hp_task_started = true;
    }
    ESP_LOGI(TAG, "A1 DAC open: %" PRIu32 " Hz, stereo, 16-bit, volume=%u",
             sample_rate, (unsigned)volume);
    return ESP_OK;
}

esp_err_t a1_audio_write(const void *data, size_t size)
{
    ESP_RETURN_ON_FALSE(s_open && data && size, ESP_ERR_INVALID_STATE, TAG,
                        "A1 DAC is not ready for PCM");
    return codec_result(esp_codec_dev_write(s_dac, (void *)data, (int)size), "write A1 DAC");
}

esp_err_t a1_audio_set_volume(uint8_t volume)
{
    ESP_RETURN_ON_FALSE(s_open, ESP_ERR_INVALID_STATE, TAG, "A1 DAC is not open");
    return codec_result(esp_codec_dev_set_out_vol(s_dac, volume), "set A1 volume");
}

esp_err_t a1_audio_set_mute(bool mute)
{
    if (!s_open) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(codec_result(esp_codec_dev_set_out_mute(s_dac, mute),
                                     mute ? "mute A1" : "unmute A1"),
                        TAG, "change A1 mute state failed");
    portENTER_CRITICAL(&s_route_lock);
    s_muted = mute;
    apply_output_route_locked();
    portEXIT_CRITICAL(&s_route_lock);
    return ESP_OK;
}
