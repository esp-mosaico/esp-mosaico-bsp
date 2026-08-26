/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PHOTO_USB_MSC_MAX_JPEG_SIZE  (256 * 1024)

#if CONFIG_CAMERA_PHOTO_USB_MSC

esp_err_t photo_usb_msc_init(void);

esp_err_t photo_usb_msc_sync_from_store(void);

esp_err_t photo_usb_msc_export_jpeg(const char *filename, const uint8_t *jpeg_data,
                                    size_t jpeg_size);

esp_err_t photo_usb_msc_remove_jpeg(const char *filename);

bool photo_usb_msc_is_host_mounted(void);

#else

static inline esp_err_t photo_usb_msc_init(void)
{
    return ESP_OK;
}

static inline esp_err_t photo_usb_msc_sync_from_store(void)
{
    return ESP_OK;
}

static inline esp_err_t photo_usb_msc_export_jpeg(const char *filename,
                                                 const uint8_t *jpeg_data,
                                                 size_t jpeg_size)
{
    (void)filename;
    (void)jpeg_data;
    (void)jpeg_size;
    return ESP_OK;
}

static inline esp_err_t photo_usb_msc_remove_jpeg(const char *filename)
{
    (void)filename;
    return ESP_OK;
}

static inline bool photo_usb_msc_is_host_mounted(void)
{
    return false;
}

#endif /* CONFIG_CAMERA_PHOTO_USB_MSC */

#ifdef __cplusplus
}
#endif
