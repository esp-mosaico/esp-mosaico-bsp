/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"
#include "pong_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the Pong UI object tree.
 *
 * This function must be called once while the caller owns the LVGL lock.
 * Passing NULL as parent uses the active screen.
 */
esp_err_t pong_ui_create(lv_obj_t *parent, pong_role_t viewport);

/**
 * @brief Select which half of the 960x480 world is shown.
 *
 * Call only from the LVGL context.
 */
void pong_ui_set_viewport(pong_role_t viewport);

/**
 * @brief Apply a render snapshot to the existing objects.
 *
 * The function creates no LVGL objects and must run from the LVGL context.
 */
void pong_ui_update(const pong_render_snapshot_t *snapshot);

/** @brief Return true after the object tree has been created. */
bool pong_ui_is_created(void);

#ifdef __cplusplus
}
#endif
