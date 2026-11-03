/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "mosaico_module_tof.h"
#include "mosaico_module_mgr.h"

esp_err_t tof_ui_init(void);
void tof_ui_status(const char *message, const char *detail);
void tof_ui_update(const mosaico_tof_data_t *data);
void tof_ui_set_slot(mosaico_module_mgr_slot_t slot, const char *model);
