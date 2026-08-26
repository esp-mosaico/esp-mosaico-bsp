/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t serial;
    uint32_t size_bytes;
    char filename[16];
} photo_store_info_t;

esp_err_t photo_store_init(void);
void photo_store_deinit(void);

uint32_t photo_store_get_count(void);

esp_err_t photo_store_get_info(uint32_t index, photo_store_info_t *out_info);

esp_err_t photo_store_save_jpeg(const uint8_t *jpeg_data, size_t jpeg_size,
                                photo_store_info_t *out_info);

esp_err_t photo_store_load_jpeg(uint32_t index, uint8_t *buffer, size_t buffer_size,
                                size_t *out_size);

esp_err_t photo_store_delete(uint32_t index);

#ifdef __cplusplus
}
#endif
