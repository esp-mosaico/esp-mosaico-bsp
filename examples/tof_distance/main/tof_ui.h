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
void tof_ui_status(mosaico_module_mgr_slot_t slot, const char *message);
void tof_ui_update(mosaico_module_mgr_slot_t slot, const vl53l0x_data_t *data);
void tof_ui_gesture(tof_gesture_event_t event, bool available, bool ready);
