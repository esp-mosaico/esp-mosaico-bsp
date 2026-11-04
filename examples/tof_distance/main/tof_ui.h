/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "mosaico_module_tof.h"
#include "mosaico_module_mgr.h"
#include "tof_gesture.h"

esp_err_t tof_ui_init(void);
/* Publish copied state without acquiring the display lock. The LVGL task renders
 * the newest state from a 50 ms timer; pending gesture events survive coalescing. */
void tof_ui_status(mosaico_module_mgr_slot_t slot, const char *message);
void tof_ui_update(mosaico_module_mgr_slot_t slot, const mosaico_tof_data_t *data);
void tof_ui_gesture(tof_gesture_event_t event, bool available, bool ready);

void tof_ui_set_sensor(mosaico_module_mgr_slot_t slot, const char *model);
