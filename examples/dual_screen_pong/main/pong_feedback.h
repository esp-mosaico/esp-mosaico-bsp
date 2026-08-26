/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pong_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PONG_FEEDBACK_CUE_PAIRED = 0,
    PONG_FEEDBACK_CUE_READY,
    PONG_FEEDBACK_CUE_PAUSED,
    PONG_FEEDBACK_CUE_RESUMED,
    PONG_FEEDBACK_CUE_CONNECTION_LOST,
    PONG_FEEDBACK_CUE_EMOTE,
} pong_feedback_cue_t;

/**
 * @brief Start the independent feedback queue and worker task.
 *
 * Motor and speaker initialization happens in the worker. Failure of either
 * output enters a degraded mode and never stops game execution.
 */
esp_err_t pong_feedback_init(void);

/**
 * @brief Queue haptic/audio feedback for a game event without blocking.
 *
 * Returns ESP_ERR_TIMEOUT when the queue is full. PONG_EVENT_NONE is ignored.
 */
esp_err_t pong_feedback_post(pong_event_kind_t event);

/** @brief Queue feedback for a UI or connection state transition. */
esp_err_t pong_feedback_post_cue(pong_feedback_cue_t cue);

/**
 * @brief Queue a custom tone without blocking.
 *
 * Frequency is clamped to 80..4000 Hz, duration to 10..500 ms, and volume to
 * 0..100. In silent degraded mode the request is accepted and discarded.
 */
esp_err_t pong_feedback_play_tone(uint16_t frequency_hz, uint16_t duration_ms,
                                  uint8_t volume_percent);

/** @brief Enable or disable each output path at runtime. */
void pong_feedback_set_enabled(bool haptic_enabled, bool audio_enabled);

/** @brief Return true when the codec output is initialized and enabled. */
bool pong_feedback_audio_available(void);

/** @brief Stop the worker task and release its queue. */
void pong_feedback_deinit(void);

#ifdef __cplusplus
}
#endif
