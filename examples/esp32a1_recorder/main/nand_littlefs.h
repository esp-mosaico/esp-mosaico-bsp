/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the BSP NAND and mount its existing LittleFS filesystem.
 *
 * Mounted read-write. A failed mount never formats the NAND contents.
 */
esp_err_t nand_littlefs_mount(const char *base_path);

#ifdef __cplusplus
}
#endif
