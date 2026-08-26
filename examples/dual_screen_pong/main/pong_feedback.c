/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pong_feedback.h"

#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define FEEDBACK_QUEUE_DEPTH       12
#define FEEDBACK_TASK_STACK        4096
#define FEEDBACK_TASK_PRIORITY     5
#define TONE_SAMPLE_RATE           16000U
#define TONE_CHUNK_SAMPLES         160U
#define TONE_RAMP_SAMPLES          80U

static const char *TAG = "pong_feedback";

typedef enum {
    FEEDBACK_COMMAND_EVENT = 0,
    FEEDBACK_COMMAND_CUE,
    FEEDBACK_COMMAND_TONE,
    FEEDBACK_COMMAND_STOP,
} feedback_command_kind_t;

typedef struct {
    feedback_command_kind_t kind;
    uint32_t posted_ms;
    union {
        pong_event_kind_t event;
        pong_feedback_cue_t cue;
        struct {
            uint16_t frequency_hz;
            uint16_t duration_ms;
            uint8_t volume_percent;
        } tone;
    };
} feedback_command_t;

typedef struct {
    uint8_t motor_strength;
    uint16_t motor_duration_ms;
    uint16_t tone_frequency_hz;
    uint16_t tone_end_frequency_hz;
    uint16_t tone_duration_ms;
    uint8_t tone_volume;
} event_feedback_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static esp_codec_dev_handle_t s_speaker;
static volatile bool s_haptic_enabled = true;
static volatile bool s_audio_enabled = true;
static volatile bool s_motor_available;
static volatile bool s_audio_available;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint16_t clamp_u16(uint16_t value, uint16_t minimum, uint16_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static uint8_t clamp_u8(uint8_t value, uint8_t maximum)
{
    return value > maximum ? maximum : value;
}

static event_feedback_t feedback_for_event(pong_event_kind_t event)
{
    switch (event) {
    case PONG_EVENT_PADDLE_HIT:
        return (event_feedback_t) {
            .motor_strength = 78, .motor_duration_ms = 58,
            .tone_frequency_hz = 760, .tone_end_frequency_hz = 620,
            .tone_duration_ms = 52, .tone_volume = 38,
        };
    case PONG_EVENT_WALL_HIT:
        return (event_feedback_t) {
            .motor_strength = 32, .motor_duration_ms = 24,
            .tone_frequency_hz = 420, .tone_end_frequency_hz = 360,
            .tone_duration_ms = 24, .tone_volume = 24,
        };
    case PONG_EVENT_SEAM_CROSS:
        return (event_feedback_t) {
            .motor_strength = 22, .motor_duration_ms = 18,
            .tone_frequency_hz = 900, .tone_end_frequency_hz = 1250,
            .tone_duration_ms = 32, .tone_volume = 20,
        };
    case PONG_EVENT_GOAL:
        return (event_feedback_t) {
            .motor_strength = 88, .motor_duration_ms = 130,
            .tone_frequency_hz = 520, .tone_end_frequency_hz = 260,
            .tone_duration_ms = 120, .tone_volume = 42,
        };
    case PONG_EVENT_SERVE:
        return (event_feedback_t) {
            .motor_strength = 38, .motor_duration_ms = 32,
            .tone_frequency_hz = 480, .tone_end_frequency_hz = 720,
            .tone_duration_ms = 42, .tone_volume = 28,
        };
    case PONG_EVENT_MATCH_WIN:
        return (event_feedback_t) {
            .motor_strength = 100, .motor_duration_ms = 220,
            .tone_frequency_hz = 620, .tone_end_frequency_hz = 1280,
            .tone_duration_ms = 210, .tone_volume = 48,
        };
    case PONG_EVENT_NONE:
    default:
        return (event_feedback_t) {0};
    }
}

static event_feedback_t feedback_for_cue(pong_feedback_cue_t cue)
{
    switch (cue) {
    case PONG_FEEDBACK_CUE_PAIRED:
        return (event_feedback_t) {
            .motor_strength = 20, .motor_duration_ms = 20,
            .tone_frequency_hz = 520, .tone_end_frequency_hz = 780,
            .tone_duration_ms = 70, .tone_volume = 24,
        };
    case PONG_FEEDBACK_CUE_READY:
        return (event_feedback_t) {
            .motor_strength = 30, .motor_duration_ms = 28,
            .tone_frequency_hz = 660, .tone_end_frequency_hz = 900,
            .tone_duration_ms = 55, .tone_volume = 26,
        };
    case PONG_FEEDBACK_CUE_PAUSED:
        return (event_feedback_t) {
            .motor_strength = 25, .motor_duration_ms = 45,
            .tone_frequency_hz = 420, .tone_end_frequency_hz = 260,
            .tone_duration_ms = 70, .tone_volume = 24,
        };
    case PONG_FEEDBACK_CUE_RESUMED:
        return (event_feedback_t) {
            .motor_strength = 32, .motor_duration_ms = 32,
            .tone_frequency_hz = 480, .tone_end_frequency_hz = 760,
            .tone_duration_ms = 65, .tone_volume = 26,
        };
    case PONG_FEEDBACK_CUE_CONNECTION_LOST:
        return (event_feedback_t) {
            .motor_strength = 55, .motor_duration_ms = 90,
            .tone_frequency_hz = 360, .tone_end_frequency_hz = 180,
            .tone_duration_ms = 120, .tone_volume = 30,
        };
    case PONG_FEEDBACK_CUE_EMOTE:
        return (event_feedback_t) {
            .motor_strength = 18, .motor_duration_ms = 18,
            .tone_frequency_hz = 880, .tone_end_frequency_hz = 1180,
            .tone_duration_ms = 55, .tone_volume = 22,
        };
    default:
        return (event_feedback_t) {0};
    }
}

static void disable_audio(const char *reason, int codec_error)
{
    s_audio_available = false;
    ESP_LOGW(TAG, "Audio feedback disabled: %s (codec error=%d)",
             reason, codec_error);
}

static void init_outputs(void)
{
    const esp_err_t motor_ret = bsp_motor_init();
    if (motor_ret == ESP_OK) {
        s_motor_available = true;
        (void)bsp_motor_set(false);
    } else {
        s_motor_available = false;
        ESP_LOGW(TAG, "Haptic feedback disabled: motor init failed (%s)",
                 esp_err_to_name(motor_ret));
    }

    /*
     * The right-slot joystick B button is GPIO40, which is also the board's
     * microphone DIN. This game only needs speaker output, so leave DIN
     * unassigned before creating the codec and keep GPIO40 available as input.
     */
    i2s_std_config_t speaker_i2s = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK,
            .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK,
            .dout = BSP_AUDIO_I2S_SDOUT,
            .din = GPIO_NUM_NC,
        },
    };
    speaker_i2s.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    const esp_err_t audio_ret = bsp_audio_init(&speaker_i2s);
    if (audio_ret != ESP_OK) {
        disable_audio("speaker-only I2S init failed", audio_ret);
        return;
    }

    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) {
        disable_audio("speaker init failed", ESP_FAIL);
        return;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = TONE_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
    };
    const int codec_ret = esp_codec_dev_open(s_speaker, &sample_info);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        disable_audio("speaker open failed", codec_ret);
        return;
    }

    s_audio_available = true;
    ESP_LOGI(TAG, "Feedback outputs ready: motor=%s audio=ready",
             s_motor_available ? "ready" : "silent");
}

static void play_tone(uint16_t frequency_hz, uint16_t end_frequency_hz,
                      uint16_t duration_ms, uint8_t volume_percent)
{
    if (!s_audio_available || !s_audio_enabled || !s_speaker ||
        volume_percent == 0U) {
        return;
    }

    frequency_hz = clamp_u16(frequency_hz, 80U, 4000U);
    end_frequency_hz = clamp_u16(end_frequency_hz, 80U, 4000U);
    duration_ms = clamp_u16(duration_ms, 10U, 500U);
    volume_percent = clamp_u8(volume_percent, 100U);
    const int volume_ret = esp_codec_dev_set_out_vol(s_speaker, volume_percent);
    if (volume_ret != ESP_CODEC_DEV_OK) {
        disable_audio("set volume failed", volume_ret);
        return;
    }

    int16_t pcm[TONE_CHUNK_SAMPLES];
    const uint32_t total_samples = (uint32_t)duration_ms * TONE_SAMPLE_RATE / 1000U;
    uint32_t phase = 0;
    uint32_t generated = 0;

    while (generated < total_samples && s_audio_available && s_audio_enabled) {
        const uint32_t count =
            (total_samples - generated) < TONE_CHUNK_SAMPLES ?
                (total_samples - generated) : TONE_CHUNK_SAMPLES;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t sample_index = generated + i;
            const int32_t frequency_span =
                (int32_t)end_frequency_hz - (int32_t)frequency_hz;
            const uint32_t current_frequency = (uint32_t)(
                (int32_t)frequency_hz +
                frequency_span * (int32_t)sample_index /
                    (int32_t)(total_samples > 1U ? total_samples - 1U : 1U));
            const uint32_t phase_step = (uint32_t)(
                ((uint64_t)current_frequency << 32) / TONE_SAMPLE_RATE);
            const uint16_t phase_u16 = (uint16_t)(phase >> 16);
            const int32_t triangle = phase_u16 < 32768U ?
                -32767 + (int32_t)phase_u16 * 2 :
                98303 - (int32_t)phase_u16 * 2;
            uint32_t envelope = TONE_RAMP_SAMPLES;
            if (sample_index < envelope) {
                envelope = sample_index;
            }
            const uint32_t remaining = total_samples - sample_index - 1U;
            if (remaining < envelope) {
                envelope = remaining;
            }
            pcm[i] = (int16_t)((triangle * 6000 * (int32_t)envelope) /
                               (32767 * (int32_t)TONE_RAMP_SAMPLES));
            phase += phase_step;
        }

        const int codec_ret = esp_codec_dev_write(
            s_speaker, pcm, (int)(count * sizeof(pcm[0])));
        if (codec_ret != ESP_CODEC_DEV_OK) {
            disable_audio("tone write failed", codec_ret);
            break;
        }
        generated += count;
    }
}

static void handle_feedback(event_feedback_t feedback)
{
    if (feedback.motor_duration_ms == 0U && feedback.tone_duration_ms == 0U) {
        return;
    }

    const bool run_motor = s_motor_available && s_haptic_enabled &&
                           feedback.motor_strength > 0U;
    if (run_motor) {
        const esp_err_t ret = bsp_motor_set_strength(feedback.motor_strength);
        if (ret != ESP_OK) {
            s_motor_available = false;
            ESP_LOGW(TAG, "Haptic feedback disabled: motor write failed (%s)",
                     esp_err_to_name(ret));
        }
    }

    play_tone(feedback.tone_frequency_hz, feedback.tone_end_frequency_hz,
              feedback.tone_duration_ms, feedback.tone_volume);

    if (run_motor && s_motor_available) {
        if (!s_audio_available || !s_audio_enabled ||
            feedback.motor_duration_ms > feedback.tone_duration_ms) {
            const uint16_t remaining_ms =
                feedback.motor_duration_ms > feedback.tone_duration_ms ?
                    feedback.motor_duration_ms - feedback.tone_duration_ms :
                    feedback.motor_duration_ms;
            vTaskDelay(pdMS_TO_TICKS(remaining_ms));
        }
        const esp_err_t ret = bsp_motor_set(false);
        if (ret != ESP_OK) {
            s_motor_available = false;
            ESP_LOGW(TAG, "Haptic feedback disabled: motor stop failed (%s)",
                     esp_err_to_name(ret));
        }
    }
}

static uint32_t command_max_age_ms(const feedback_command_t *command)
{
    if (command->kind == FEEDBACK_COMMAND_EVENT) {
        switch (command->event) {
        case PONG_EVENT_PADDLE_HIT:
        case PONG_EVENT_WALL_HIT:
        case PONG_EVENT_SEAM_CROSS:
            return 100U;
        case PONG_EVENT_SERVE:
            return 200U;
        default:
            return 600U;
        }
    }
    return command->kind == FEEDBACK_COMMAND_STOP ? UINT32_MAX : 500U;
}

static void feedback_task(void *arg)
{
    (void)arg;
    init_outputs();

    feedback_command_t command;
    bool running = true;
    while (running && xQueueReceive(s_queue, &command, portMAX_DELAY) == pdTRUE) {
        const uint32_t age_ms = now_ms() - command.posted_ms;
        if (age_ms > command_max_age_ms(&command)) {
            ESP_LOGD(TAG, "Dropped stale feedback: kind=%u age=%ums",
                     (unsigned)command.kind, (unsigned)age_ms);
            continue;
        }
        switch (command.kind) {
        case FEEDBACK_COMMAND_EVENT:
            handle_feedback(feedback_for_event(command.event));
            break;
        case FEEDBACK_COMMAND_CUE:
            handle_feedback(feedback_for_cue(command.cue));
            break;
        case FEEDBACK_COMMAND_TONE:
            play_tone(command.tone.frequency_hz, command.tone.frequency_hz,
                      command.tone.duration_ms, command.tone.volume_percent);
            break;
        case FEEDBACK_COMMAND_STOP:
            running = false;
            break;
        default:
            break;
        }
    }

    if (s_motor_available) {
        (void)bsp_motor_set(false);
    }
    if (s_speaker && s_audio_available) {
        (void)esp_codec_dev_close(s_speaker);
    }
    s_audio_available = false;
    s_motor_available = false;
    s_speaker = NULL;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t pong_feedback_init(void)
{
    if (s_queue || s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    s_haptic_enabled = true;
    s_audio_enabled = true;
    s_audio_available = false;
    s_motor_available = false;
    s_queue = xQueueCreate(FEEDBACK_QUEUE_DEPTH, sizeof(feedback_command_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Create feedback queue failed (%s)",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(feedback_task, "pong_feedback", FEEDBACK_TASK_STACK, NULL,
                    FEEDBACK_TASK_PRIORITY, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "Create feedback task failed (%s)",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Feedback worker started with queue depth %u",
             (unsigned)FEEDBACK_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t pong_feedback_post(pong_event_kind_t event)
{
    if (event == PONG_EVENT_NONE) {
        return ESP_OK;
    }
    if (!s_queue || !s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    const feedback_command_t command = {
        .kind = FEEDBACK_COMMAND_EVENT,
        .posted_ms = now_ms(),
        .event = event,
    };
    const bool critical = event == PONG_EVENT_GOAL ||
                          event == PONG_EVENT_MATCH_WIN;
    const BaseType_t queued = critical ?
        xQueueSendToFront(s_queue, &command, 0) :
        xQueueSend(s_queue, &command, 0);
    return queued == pdTRUE ?
        ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t pong_feedback_post_cue(pong_feedback_cue_t cue)
{
    if (!s_queue || !s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cue < PONG_FEEDBACK_CUE_PAIRED ||
        cue > PONG_FEEDBACK_CUE_EMOTE) {
        return ESP_ERR_INVALID_ARG;
    }

    const feedback_command_t command = {
        .kind = FEEDBACK_COMMAND_CUE,
        .posted_ms = now_ms(),
        .cue = cue,
    };
    const bool critical = cue == PONG_FEEDBACK_CUE_PAUSED ||
                          cue == PONG_FEEDBACK_CUE_CONNECTION_LOST;
    const BaseType_t queued = critical ?
        xQueueSendToFront(s_queue, &command, 0) :
        xQueueSend(s_queue, &command, 0);
    return queued == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t pong_feedback_play_tone(uint16_t frequency_hz, uint16_t duration_ms,
                                  uint8_t volume_percent)
{
    if (!s_queue || !s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    const feedback_command_t command = {
        .kind = FEEDBACK_COMMAND_TONE,
        .posted_ms = now_ms(),
        .tone = {
            .frequency_hz = clamp_u16(frequency_hz, 80U, 4000U),
            .duration_ms = clamp_u16(duration_ms, 10U, 500U),
            .volume_percent = clamp_u8(volume_percent, 100U),
        },
    };
    return xQueueSend(s_queue, &command, 0) == pdTRUE ?
        ESP_OK : ESP_ERR_TIMEOUT;
}

void pong_feedback_set_enabled(bool haptic_enabled, bool audio_enabled)
{
    s_haptic_enabled = haptic_enabled;
    s_audio_enabled = audio_enabled;
}

bool pong_feedback_audio_available(void)
{
    return s_audio_available && s_audio_enabled;
}

void pong_feedback_deinit(void)
{
    if (!s_queue) {
        return;
    }

    const feedback_command_t command = {
        .kind = FEEDBACK_COMMAND_STOP,
        .posted_ms = now_ms(),
    };
    if (s_task) {
        xQueueReset(s_queue);
        (void)xQueueSendToFront(s_queue, &command, portMAX_DELAY);
        for (uint32_t waited_ms = 0; s_task && waited_ms < 1200U; waited_ms += 10U) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!s_task) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGI(TAG, "Feedback worker stopped");
    } else {
        ESP_LOGW(TAG, "Feedback worker stop timed out; resources retained");
    }
}
