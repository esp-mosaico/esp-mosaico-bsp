/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "music_player.h"

typedef void (*music_ui_command_callback_t)(music_player_command_t command,
                                            void *user_data);

esp_err_t music_ui_start(music_ui_command_callback_t callback, void *user_data);
void music_ui_update(const music_player_state_t *state);
void music_ui_set_error(const char *message);
void music_ui_loading_stage(const char *message);
void music_ui_finish_loading(void);
