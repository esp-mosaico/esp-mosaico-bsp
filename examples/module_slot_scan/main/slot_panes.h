/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "mosaico_module_mgr.h"

#define SLOT_PANE_WIDTH  240
#define SLOT_PANE_HEIGHT 480

typedef struct slot_pane slot_pane_t;

void slot_panes_create(lv_obj_t *parent, slot_pane_t **out_left, slot_pane_t **out_right);
void slot_pane_show_empty(slot_pane_t *pane);
void slot_pane_show_other(slot_pane_t *pane, const char *name);
esp_err_t slot_pane_start_camera(slot_pane_t *pane);
esp_err_t slot_pane_start_interact(slot_pane_t *pane);
void slot_pane_stop(slot_pane_t *pane);
