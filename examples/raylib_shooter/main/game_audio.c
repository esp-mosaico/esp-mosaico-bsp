// SPDX-License-Identifier: Apache-2.0
#include "game_audio.h"

#include <math.h>
#include <stdint.h>
#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SAMPLE_RATE 16000U
#define CHUNK_SAMPLES 160U

typedef struct {
    uint16_t start_hz;
    uint16_t end_hz;
    uint16_t duration_ms;
    uint8_t volume;
} tone_t;

static const char *TAG = "game_audio";
static QueueHandle_t s_queue;
static esp_codec_dev_handle_t s_speaker;
static volatile bool s_available;

static tone_t cue_tone(game_audio_cue_t cue)
{
    switch (cue) {
    case GAME_AUDIO_START: return (tone_t){660, 880, 120, 52};
    case GAME_AUDIO_SHOT: return (tone_t){980, 520, 32, 46};
    case GAME_AUDIO_DESTROY: return (tone_t){440, 740, 85, 54};
    case GAME_AUDIO_HIT: return (tone_t){170, 105, 145, 58};
    case GAME_AUDIO_GAME_OVER: return (tone_t){360, 180, 300, 56};
    case GAME_AUDIO_ENGINE: return (tone_t){0};
    default: return (tone_t){0};
    }
}

static void disable_audio(const char *reason, int error)
{
    s_available = false;
    ESP_LOGW(TAG, "audio disabled: %s (%d)", reason, error);
}

static void play_tone(tone_t tone)
{
    if (!s_available || tone.duration_ms == 0) return;
    if (esp_codec_dev_set_out_vol(s_speaker, tone.volume) != ESP_CODEC_DEV_OK) {
        disable_audio("volume", ESP_FAIL);
        return;
    }
    int16_t pcm[CHUNK_SAMPLES];
    uint32_t phase = 0;
    const uint32_t total = (uint32_t)tone.duration_ms * SAMPLE_RATE / 1000U;
    for (uint32_t generated = 0; generated < total;) {
        uint32_t count = total - generated;
        if (count > CHUNK_SAMPLES) count = CHUNK_SAMPLES;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t at = generated + i;
            int32_t span = (int32_t)tone.end_hz - tone.start_hz;
            uint32_t hz = (uint32_t)((int32_t)tone.start_hz +
                span * (int32_t)at / (int32_t)(total > 1 ? total - 1 : 1));
            phase += (uint32_t)(((uint64_t)hz << 32) / SAMPLE_RATE);
            const float angle = (float)phase * 1.46291808e-9f;
            const float wave = sinf(angle) + 0.06f * sinf(angle * 2.0f);
            uint32_t edge = at < 80 ? at : 80;
            uint32_t remaining = total - at - 1;
            if (remaining < edge) edge = remaining;
            pcm[i] = (int16_t)(wave * 12000.0f * (float)edge / 80.0f);
        }
        if (esp_codec_dev_write(s_speaker, pcm,
                (int)(count * sizeof(pcm[0]))) != ESP_CODEC_DEV_OK) {
            disable_audio("write", ESP_FAIL);
            return;
        }
        generated += count;
    }
}

static void play_cue(game_audio_cue_t cue)
{
    play_tone(cue_tone(cue));
}

static void audio_task(void *arg)
{
    (void)arg;
    i2s_std_config_t i2s = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = BSP_AUDIO_I2S_MCLK, .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK, .dout = BSP_AUDIO_I2S_SDOUT,
            .din = GPIO_NUM_NC},
    };
    i2s.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    esp_err_t err = bsp_audio_init(&i2s);
    if (err != ESP_OK) {
        disable_audio("I2S init", err);
    } else {
        s_speaker = bsp_audio_codec_speaker_init();
        esp_codec_dev_sample_info_t info = {
            .sample_rate = SAMPLE_RATE, .bits_per_sample = 16,
            .channel = 1, .channel_mask = 0,
        };
        if (!s_speaker || esp_codec_dev_open(s_speaker, &info) != ESP_CODEC_DEV_OK) {
            disable_audio("codec open", ESP_FAIL);
        } else {
            if (esp_codec_dev_set_out_mute(s_speaker, false) != ESP_CODEC_DEV_OK) {
                disable_audio("unmute", ESP_FAIL);
            } else {
                s_available = true;
                ESP_LOGI(TAG, "ES8311 game audio ready at %u Hz", SAMPLE_RATE);
            }
        }
    }
    game_audio_cue_t cue;
    while (xQueueReceive(s_queue, &cue, portMAX_DELAY) == pdTRUE) {
        play_cue(cue);
    }
}

esp_err_t game_audio_init(void)
{
    if (s_queue) return ESP_ERR_INVALID_STATE;
    s_queue = xQueueCreate(12, sizeof(game_audio_cue_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(audio_task, "game_audio", 4096, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t game_audio_play(game_audio_cue_t cue)
{
    if (!s_queue || cue > GAME_AUDIO_ENGINE) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_queue, &cue, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool game_audio_available(void)
{
    return s_available;
}
