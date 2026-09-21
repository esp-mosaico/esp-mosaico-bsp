/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    MUSIC_PLAYER_PREVIOUS = 0,
    MUSIC_PLAYER_TOGGLE,
    MUSIC_PLAYER_NEXT,
    MUSIC_PLAYER_VOLUME_DOWN,
    MUSIC_PLAYER_VOLUME_UP,
} music_player_command_t;

typedef struct {
    size_t track_index;
    size_t track_count;
    const char *title;
    const char *artist;
    uint8_t volume;
    bool playing;
} music_player_state_t;

typedef void (*music_player_state_callback_t)(const music_player_state_t *state,
                                              void *user_data);

esp_err_t music_player_start(music_player_state_callback_t callback, void *user_data);
esp_err_t music_player_send(music_player_command_t command);
void music_player_get_state(music_player_state_t *state);
