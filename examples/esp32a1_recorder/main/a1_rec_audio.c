/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "a1_rec_audio.h"
#include "bsp/esp_mosaico.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "esp32_a1_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#define A1_POWER_GPIO          GPIO_NUM_10
#define A1_VCC_3V3_GPIO        GPIO_NUM_60
#define A1_VCC_3V3_ON_LEVEL    0
#define A1_PA_GPIO             GPIO_NUM_38
#define A1_I2C_SDA_GPIO        GPIO_NUM_0
#define A1_I2C_SCL_GPIO        GPIO_NUM_1
#define A1_I2S_MCLK_GPIO       GPIO_NUM_54
#define A1_I2S_BCLK_GPIO       GPIO_NUM_37
#define A1_I2S_WS_GPIO         GPIO_NUM_49
/* A1 I2S_SDIN is GPIO52; I2S_SDOUT is GPIO40. Opposite of onboard ES8311. */
#define A1_I2S_DOUT_GPIO       GPIO_NUM_52
#define A1_I2S_DIN_GPIO        GPIO_NUM_40
#define A1_I2C_ADDR_8BIT       (0x58)
#define A1_SAMPLE_RATE         (48000)
#define A1_BITS_PER_SAMPLE     (24)
#define A1_FRAME_SLOTS         (5)
#define A1_DAC_CHANNELS        (2)
#define A1_DAC_CHANNEL_MASK    (0x03)
#define A1_ADC_CHANNEL_MASK    (0x1F)
#define A1_MCLK_MULTIPLE       (960)
#define A1_BYTES_PER_SAMPLE    ((A1_BITS_PER_SAMPLE + 7) / 8)
#define A1_ADC_FRAME_BYTES     (A1_FRAME_SLOTS * A1_BYTES_PER_SAMPLE)
#define A1_DAC_FRAME_BYTES     (A1_DAC_CHANNELS * A1_BYTES_PER_SAMPLE)
#define LOOP_FRAMES            (256)
#define DAC_VOLUME             (60)
#define ADC_GAIN_DB            (10.0f)
#define DAC_PRIME_WRITES       (8)

static const char *TAG = "a1_record_playback";

static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_dac_codec_if;
static const audio_codec_if_t *s_adc_codec_if;
static esp_codec_dev_handle_t s_adc_dev;
static esp_codec_dev_handle_t s_dac_dev;
static i2c_master_bus_handle_t s_i2c_bus;

static esp_err_t codec_result(int result, const char *operation)
{
    if (result == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed: %d", operation, result);
    return ESP_FAIL;
}

static esp_err_t a1_hold_i2s_strap_low(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << A1_I2S_MCLK_GPIO) | (1ULL << A1_I2S_BCLK_GPIO) |
                        (1ULL << A1_I2S_WS_GPIO) | (1ULL << A1_I2S_DOUT_GPIO) |
                        (1ULL << A1_I2S_DIN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure A1 I2S strap GPIOs failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_MCLK_GPIO, 0), TAG, "hold MCLK low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_BCLK_GPIO, 0), TAG, "hold BCLK low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_WS_GPIO, 0), TAG, "hold WS low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_DOUT_GPIO, 0), TAG, "hold DOUT low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_I2S_DIN_GPIO, 0), TAG, "hold DIN low failed");
    return ESP_OK;
}

static void a1_release_i2s_pins(void)
{
    gpio_reset_pin(A1_I2S_MCLK_GPIO);
    gpio_reset_pin(A1_I2S_BCLK_GPIO);
    gpio_reset_pin(A1_I2S_WS_GPIO);
    gpio_reset_pin(A1_I2S_DOUT_GPIO);
    gpio_reset_pin(A1_I2S_DIN_GPIO);
}

static esp_err_t a1_power_on(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << A1_POWER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure A1 power GPIO failed");
    /* H1 VCC_3V3 is gated by Mosaico GPIO60, active-low. A1 digital/I2C dies without it. */
    ESP_RETURN_ON_ERROR(bsp_subboard_init(), TAG, "initialize expansion I2C1 failed");
    /* CHIP_PU follows the analog LDO. Sample strapping only on this rising edge. */
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_POWER_GPIO, 0), TAG, "hold A1 in reset failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(gpio_set_level(A1_POWER_GPIO, 1), TAG, "enable A1 codec LDO failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t a1_i2s_init(void)
{
    a1_release_i2s_pins();
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 60;
    channel_config.dma_frame_num = 128;
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx_channel, &s_rx_channel), TAG,
                        "create I2S channels failed");

    i2s_tdm_config_t config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(A1_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_24BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1),
        .gpio_cfg = {
            .mclk = A1_I2S_MCLK_GPIO,
            .bclk = A1_I2S_BCLK_GPIO,
            .ws = A1_I2S_WS_GPIO,
            .dout = A1_I2S_DOUT_GPIO,
            .din = A1_I2S_DIN_GPIO,
            .invert_flags = {0},
        },
    };
    config.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    config.clk_cfg.mclk_multiple = A1_MCLK_MULTIPLE;
    config.slot_cfg.total_slot = A1_FRAME_SLOTS;
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_tx_channel, &config), TAG,
                        "initialize I2S TX failed");

    config.slot_cfg.slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 |
                                I2S_TDM_SLOT3 | I2S_TDM_SLOT4;
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx_channel, &config), TAG,
                        "initialize I2S RX failed");
    return ESP_OK;
}

static void fill_codec_common(esp32_a1_codec_cfg_t *codec_config)
{
    memset(codec_config, 0, sizeof(*codec_config));
    codec_config->ctrl_if = s_ctrl_if;
    codec_config->gpio_if = s_gpio_if;
    codec_config->sys_cfg.no_mclk = true;
    codec_config->dac_cfg.ref_enable = true;
    codec_config->dac_cfg.ref_dac_ch = 1;
    codec_config->int_cfg.int_pin = -1;
}

static esp_err_t a1_codec_init(void)
{
    s_i2c_bus = bsp_subboard_get_i2c_bus();
    ESP_RETURN_ON_FALSE(s_i2c_bus, ESP_ERR_INVALID_STATE, TAG, "external bus unavailable");

    audio_codec_i2c_cfg_t i2c_config = {
        .addr = A1_I2C_ADDR_8BIT,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = 400000,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    ESP_RETURN_ON_FALSE(s_ctrl_if, ESP_FAIL, TAG, "create A1 I2C control interface failed");

    audio_codec_i2s_cfg_t i2s_config = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx_channel,
        .tx_handle = s_tx_channel,
        .clk_src = I2S_CLK_SRC_APLL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_config);
    ESP_RETURN_ON_FALSE(s_data_if, ESP_FAIL, TAG, "create A1 I2S data interface failed");
    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if, ESP_FAIL, TAG, "create codec GPIO interface failed");

    esp32_a1_codec_cfg_t dac_codec_config;
    fill_codec_common(&dac_codec_config);
    dac_codec_config.pa_cfg.pa_pin = -1; /* App owns PA for record/play transitions. */
    dac_codec_config.pa_cfg.pa_active_low = false;
    dac_codec_config.pa_cfg.hw_gain.pa_voltage = 3.3f;
    dac_codec_config.pa_cfg.hw_gain.codec_dac_voltage = 3.3f;
    dac_codec_config.pa_cfg.hw_gain.pa_gain = 0.0f;
    s_dac_codec_if = esp32_a1_codec_new(&dac_codec_config);
    ESP_RETURN_ON_FALSE(s_dac_codec_if, ESP_FAIL, TAG, "create A1 DAC codec interface failed");

    /* ADC must not own the PA pin. Opening that instance would mute the speaker. */
    esp32_a1_codec_cfg_t adc_codec_config;
    fill_codec_common(&adc_codec_config);
    adc_codec_config.adc_cfg.label = "FL,FR,BL,BR,RE";
    adc_codec_config.pa_cfg.pa_pin = -1;
    s_adc_codec_if = esp32_a1_codec_new(&adc_codec_config);
    ESP_RETURN_ON_FALSE(s_adc_codec_if, ESP_FAIL, TAG, "create A1 ADC codec interface failed");

    esp_codec_dev_cfg_t dac_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_dac_codec_if,
        .data_if = s_data_if,
    };
    esp_codec_dev_cfg_t adc_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_adc_codec_if,
        .data_if = s_data_if,
    };
    s_dac_dev = esp_codec_dev_new(&dac_config);
    s_adc_dev = esp_codec_dev_new(&adc_config);
    ESP_RETURN_ON_FALSE(s_adc_dev && s_dac_dev, ESP_FAIL, TAG, "create codec devices failed");
    return ESP_OK;
}


static esp_err_t result(int ret) { return codec_result(ret, "A1 stream"); }

esp_err_t a1_rec_audio_set_volume(unsigned volume)
{
    if (!s_dac_dev || volume > 100) return ESP_ERR_INVALID_ARG;
    return codec_result(esp_codec_dev_set_out_vol(s_dac_dev, (int)volume), "set playback volume");
}

void a1_rec_audio_speaker(bool enabled)
{
    /* Headphone detect is active-low. ADC and idle must never enable PA. */
    gpio_set_level(A1_PA_GPIO, enabled && gpio_get_level(GPIO_NUM_47));
    (void)esp_codec_dev_set_out_mute(s_dac_dev, !enabled);
}

void a1_rec_audio_update_route(bool playing)
{
    gpio_set_level(A1_PA_GPIO, playing && gpio_get_level(GPIO_NUM_47));
}

esp_err_t a1_rec_audio_init(void)
{
    const gpio_config_t pa = {.pin_bit_mask = 1ULL << A1_PA_GPIO, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&pa), TAG, "PA gpio");
    gpio_set_level(A1_PA_GPIO, 0);
    const gpio_config_t hp = {.pin_bit_mask = 1ULL << GPIO_NUM_47, .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&hp), TAG, "headphone gpio");
    ESP_RETURN_ON_ERROR(a1_hold_i2s_strap_low(), TAG, "boot straps");
    ESP_RETURN_ON_ERROR(a1_power_on(), TAG, "A1 power");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bsp_subboard_get_i2c_bus(), 0x2c, 100),
                        TAG, "A1 not found on expansion I2C1");
    ESP_RETURN_ON_ERROR(a1_i2s_init(), TAG, "I2S");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(a1_codec_init(), TAG, "codec");

    esp_codec_dev_sample_info_t dac_fs = {
        .sample_rate = A1_SAMPLE_RATE, .bits_per_sample = A1_BITS_PER_SAMPLE,
        .channel = 2, .channel_mask = 0x03, .mclk_multiple = A1_MCLK_MULTIPLE,
    };
    esp_codec_dev_sample_info_t adc_fs = {
        .sample_rate = A1_SAMPLE_RATE, .bits_per_sample = A1_BITS_PER_SAMPLE,
        .channel = 5, .channel_mask = 0x1f, .mclk_multiple = A1_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_ERROR(result(esp_codec_dev_open(s_dac_dev, &dac_fs)), TAG, "open DAC");
    ESP_RETURN_ON_ERROR(result(esp_codec_dev_set_out_vol(s_dac_dev, DAC_VOLUME)), TAG, "volume");
    a1_rec_audio_speaker(false);
    uint8_t silence[LOOP_FRAMES * A1_DAC_FRAME_BYTES] = {0};
    for (int i = 0; i < DAC_PRIME_WRITES; ++i) {
        ESP_RETURN_ON_ERROR(result(esp_codec_dev_write(s_dac_dev, silence, sizeof(silence))),
                            TAG, "prime DAC");
    }
    ESP_RETURN_ON_ERROR(result(esp_codec_dev_open(s_adc_dev, &adc_fs)), TAG, "open ADC");
    ESP_RETURN_ON_ERROR(result(esp_codec_dev_set_in_gain(s_adc_dev, ADC_GAIN_DB)), TAG, "gain");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "A1 recorder ready: 48000 Hz, 24-bit, 5-slot TDM, MCLK=960fs");
    return ESP_OK;
}

esp_err_t a1_rec_audio_read(uint8_t *five_channel_pcm, size_t bytes)
{
    return result(esp_codec_dev_read(s_adc_dev, five_channel_pcm, bytes));
}

esp_err_t a1_rec_audio_write(uint8_t *stereo_pcm, size_t bytes)
{
    return result(esp_codec_dev_write(s_dac_dev, stereo_pcm, bytes));
}
