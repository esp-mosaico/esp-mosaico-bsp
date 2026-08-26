/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "pong_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pong_game_start(void);
void pong_game_stop(void);

/**
 * Copy the latest render state. Safe to call from the LVGL timer context.
 */
bool pong_game_get_render_snapshot(pong_render_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
