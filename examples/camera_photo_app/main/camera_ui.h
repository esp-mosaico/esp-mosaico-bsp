/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_UI_THUMB_SIZE  56
#define CAMERA_UI_EDGE_MARGIN 44

typedef void (*camera_ui_capture_cb_t)(void *user_data);
typedef void (*camera_ui_flash_toggle_cb_t)(bool enabled, void *user_data);
typedef void (*camera_ui_preview_flip_cb_t)(void *user_data);
typedef void (*camera_ui_gallery_open_cb_t)(void *user_data);
typedef void (*camera_ui_gallery_close_cb_t)(void *user_data);
typedef void (*camera_ui_gallery_nav_cb_t)(int direction, void *user_data);
typedef void (*camera_ui_gallery_delete_cb_t)(void *user_data);

typedef struct {
    camera_ui_capture_cb_t on_capture;
    camera_ui_flash_toggle_cb_t on_flash_toggle;
    camera_ui_preview_flip_cb_t on_preview_flip;
    camera_ui_gallery_open_cb_t on_gallery_open;
    camera_ui_gallery_close_cb_t on_gallery_close;
    camera_ui_gallery_nav_cb_t on_gallery_nav;
    camera_ui_gallery_delete_cb_t on_gallery_delete;
    void *user_data;
} camera_ui_callbacks_t;

esp_err_t camera_ui_create(lv_display_t *display, uint16_t *preview_buffer,
                           int preview_width, int preview_height,
                           const camera_ui_callbacks_t *callbacks);

void camera_ui_set_flash_enabled(bool enabled);
void camera_ui_set_preview_flip(bool flipped);
void camera_ui_set_camera_power_on(bool powered_on);
void camera_ui_invalidate_preview(void);
void camera_ui_play_capture_flash(void);

typedef struct {
    uint32_t async_ok;
    uint32_t async_fail;
    uint32_t coalesced;
} camera_ui_preview_refresh_stats_t;

void camera_ui_get_preview_refresh_stats(
    camera_ui_preview_refresh_stats_t *out_stats);
void camera_ui_reset_preview_refresh_stats(void);

void camera_ui_set_thumb(const uint16_t *rgb565, int width, int height);
void camera_ui_clear_thumb(void);

void camera_ui_show_camera(void);
void camera_ui_show_gallery(void);

void camera_ui_set_gallery_image(const uint16_t *rgb565, int width, int height);
void camera_ui_set_gallery_title(const char *filename);

#ifdef __cplusplus
}
#endif
