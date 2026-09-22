// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    GAME_AUDIO_START = 0,
    GAME_AUDIO_SHOT,
    GAME_AUDIO_DESTROY,
    GAME_AUDIO_HIT,
    GAME_AUDIO_GAME_OVER,
    GAME_AUDIO_ENGINE,
} game_audio_cue_t;

esp_err_t game_audio_init(void);
esp_err_t game_audio_play(game_audio_cue_t cue);
bool game_audio_available(void);
