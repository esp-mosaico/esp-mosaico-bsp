/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_ui.h"
#include "ui_icons.h"

#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "esp_log.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "widgets/canvas/lv_canvas.h"

static const char *TAG = "camera_ui";

#define SCREEN_W              BSP_LCD_H_RES
#define SCREEN_H              BSP_LCD_V_RES
#define SWIPE_THRESHOLD_PX    40
#define UI_EDGE_MARGIN        CAMERA_UI_EDGE_MARGIN
#define UI_BOTTOM_MARGIN      28
#define UI_TOP_MARGIN         UI_EDGE_MARGIN
#define UI_INFO_TOP_MARGIN    32
#define UI_ICON_SIZE          32
#define UI_INFO_ICON_SIZE     24
#define UI_INFO_HIT_SIZE      44
#define UI_TOP_BTN_GAP        4
#define GALLERY_TOP_BAR_H     64
#define GALLERY_BAR_ICON_SIZE 32
#define GALLERY_BAR_HIT_SIZE  48
#define GALLERY_BAR_SIDE_MARGIN UI_EDGE_MARGIN
#define GALLERY_TITLE_FONT_SCALE ((24 * 256) / 14)
#define SIDE_BTN_SIZE         68
#define SHUTTER_OUTER_SIZE    96
#define SHUTTER_INNER_SIZE    68
#define COLOR_BTN_BG          lv_color_hex(0x1A1A1A)
#define COLOR_PANEL           lv_color_hex(0x202020)
#define COLOR_GALLERY_BAR_BG  lv_color_hex(0xC8C8C8)
#define GALLERY_BAR_BG_OPA    LV_OPA_50
#define CAPTURE_FLASH_MS      100

typedef struct {
    lv_obj_t *camera_screen;
    lv_obj_t *gallery_screen;
    lv_obj_t *preview_canvas;
    lv_obj_t *flash_btn_img;
    lv_obj_t *orientation_label;
    lv_obj_t *gallery_thumb;
    lv_obj_t *gallery_placeholder;
    lv_obj_t *gallery_img;
    lv_obj_t *gallery_back_btn;
    lv_obj_t *gallery_delete_btn;
    lv_obj_t *gallery_title;
    lv_obj_t *delete_confirm_panel;
    lv_obj_t *info_panel;
    lv_obj_t *camera_off_panel;
    lv_obj_t *capture_flash_panel;
    lv_timer_t *capture_flash_timer;
    uint16_t *preview_buffer;
    int preview_width;
    int preview_height;
    char gallery_title_text[40];
    uint16_t thumb_buffer[CAMERA_UI_THUMB_SIZE * CAMERA_UI_THUMB_SIZE];
    lv_image_dsc_t thumb_dsc;
    lv_image_dsc_t gallery_dsc;
    bool flash_enabled;
    bool preview_flip;
    bool thumb_valid;
    int16_t swipe_start_x;
    bool swipe_tracking;
    camera_ui_callbacks_t callbacks;
} camera_ui_ctx_t;

static camera_ui_ctx_t s_ui;
static camera_ui_preview_refresh_stats_t s_preview_refresh_stats;
static bool s_preview_refresh_pending;

static void preview_refresh_canvas(void)
{
    lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(s_ui.preview_canvas);
    if (draw_buf) {
        lv_draw_buf_invalidate_cache(draw_buf, NULL);
        lv_image_cache_drop(draw_buf);
    }
    lv_obj_invalidate(s_ui.preview_canvas);
}

static void apply_flash_button_icon(void)
{
    if (!s_ui.flash_btn_img) {
        return;
    }

    if (s_ui.flash_enabled) {
        lv_image_set_src(s_ui.flash_btn_img, &icon_flash_on);
    } else {
        lv_image_set_src(s_ui.flash_btn_img, &icon_flash_off);
    }
}

static void style_side_button(lv_obj_t *btn, int size)
{
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, COLOR_BTN_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_icon_label(lv_obj_t *label, const lv_font_t *font)
{
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, font, 0);
}

static lv_obj_t *create_image_icon_button(lv_obj_t *parent,
                                          const lv_image_dsc_t *icon,
                                          lv_event_cb_t callback)
{
    lv_obj_t *btn = lv_button_create(parent);
    style_side_button(btn, SIDE_BTN_SIZE);
    if (callback) {
        lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, icon);
    lv_obj_set_size(img, UI_ICON_SIZE, UI_ICON_SIZE);
    lv_obj_center(img);
    return btn;
}

static lv_obj_t *create_plain_image_button(lv_obj_t *parent,
                                           const lv_image_dsc_t *icon,
                                           int icon_size,
                                           int hit_size,
                                           lv_event_cb_t callback)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, hit_size, hit_size);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (callback) {
        lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, icon);
    lv_obj_set_size(img, icon_size, icon_size);
    lv_obj_center(img);
    return btn;
}

static void preview_invalidate_async(void *arg)
{
    (void)arg;
    s_preview_refresh_pending = false;
    preview_refresh_canvas();
    ++s_preview_refresh_stats.async_ok;
}

static void capture_flash_hide_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_ui.capture_flash_panel) {
        lv_obj_add_flag(s_ui.capture_flash_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.capture_flash_timer) {
        lv_timer_delete(s_ui.capture_flash_timer);
        s_ui.capture_flash_timer = NULL;
    }
}

static void capture_flash_play_async(void *arg)
{
    (void)arg;
    if (!s_ui.capture_flash_panel) {
        return;
    }

    if (s_ui.capture_flash_timer) {
        lv_timer_delete(s_ui.capture_flash_timer);
        s_ui.capture_flash_timer = NULL;
    }

    lv_obj_remove_flag(s_ui.capture_flash_panel, LV_OBJ_FLAG_HIDDEN);
    s_ui.capture_flash_timer =
        lv_timer_create(capture_flash_hide_timer_cb, CAPTURE_FLASH_MS, NULL);
    lv_timer_set_repeat_count(s_ui.capture_flash_timer, 1);
}

static void apply_camera_power_ui(bool powered_on)
{
    if (!s_ui.camera_off_panel || !s_ui.preview_canvas) {
        return;
    }

    if (powered_on) {
        lv_obj_remove_flag(s_ui.preview_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.camera_off_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_ui.preview_canvas);
        return;
    }

    lv_obj_add_flag(s_ui.preview_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui.camera_off_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.camera_off_panel);
    lv_obj_invalidate(s_ui.camera_off_panel);
}

static void camera_power_apply_async(void *arg)
{
    apply_camera_power_ui((uintptr_t)arg == 1U);
}

static void thumb_apply_async(void *arg)
{
    (void)arg;
    if (s_ui.thumb_valid) {
        lv_image_set_src(s_ui.gallery_thumb, &s_ui.thumb_dsc);
        lv_obj_remove_flag(s_ui.gallery_thumb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.gallery_placeholder, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(s_ui.gallery_thumb, NULL);
        lv_obj_add_flag(s_ui.gallery_thumb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.gallery_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void gallery_image_apply_async(void *arg)
{
    (void)arg;
    lv_image_set_src(s_ui.gallery_img, &s_ui.gallery_dsc);
    lv_obj_invalidate(s_ui.gallery_img);
}

static void gallery_title_apply_async(void *arg)
{
    (void)arg;
    if (s_ui.gallery_title) {
        lv_label_set_text(s_ui.gallery_title, s_ui.gallery_title_text);
    }
}

static void screen_load_async(void *arg)
{
    lv_obj_t *screen = arg;
    lv_screen_load(screen);
}

static void flash_apply_async(void *arg)
{
    (void)arg;
    apply_flash_button_icon();
}

static void apply_orientation_label(void)
{
    if (!s_ui.orientation_label) {
        return;
    }

    lv_label_set_text(
        s_ui.orientation_label, s_ui.preview_flip ? "Front" : "Rear");
}

static void orientation_apply_async(void *arg)
{
    (void)arg;
    apply_orientation_label();
}

static void hide_delete_confirm(void)
{
    if (s_ui.delete_confirm_panel) {
        lv_obj_add_flag(s_ui.delete_confirm_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_info_panel(void)
{
    if (s_ui.info_panel) {
        lv_obj_add_flag(s_ui.info_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void delete_confirm_cancel_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    hide_delete_confirm();
}

static void delete_confirm_ok_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    hide_delete_confirm();
    if (s_ui.callbacks.on_gallery_delete) {
        s_ui.callbacks.on_gallery_delete(s_ui.callbacks.user_data);
    }
}

static void info_close_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    hide_info_panel();
}

static void info_open_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_ui.info_panel) {
        return;
    }
    lv_obj_remove_flag(s_ui.info_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.info_panel);
}

static void gallery_delete_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_ui.delete_confirm_panel) {
        return;
    }
    lv_obj_remove_flag(s_ui.delete_confirm_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.delete_confirm_panel);
}

static void shutter_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_ui.callbacks.on_capture) {
        s_ui.callbacks.on_capture(s_ui.callbacks.user_data);
    }
}

static void gallery_open_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_ui.callbacks.on_gallery_open) {
        s_ui.callbacks.on_gallery_open(s_ui.callbacks.user_data);
    }
}

static void flash_toggle_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    s_ui.flash_enabled = !s_ui.flash_enabled;
    apply_flash_button_icon();
    if (s_ui.callbacks.on_flash_toggle) {
        s_ui.callbacks.on_flash_toggle(s_ui.flash_enabled, s_ui.callbacks.user_data);
    }
}

static void preview_flip_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_ui.callbacks.on_preview_flip) {
        s_ui.callbacks.on_preview_flip(s_ui.callbacks.user_data);
    }
}

static void gallery_close_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    hide_delete_confirm();
    if (s_ui.callbacks.on_gallery_close) {
        s_ui.callbacks.on_gallery_close(s_ui.callbacks.user_data);
    }
}

static void gallery_swipe_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_point_t point = {0};
        lv_indev_get_point(indev, &point);
        s_ui.swipe_start_x = point.x;
        s_ui.swipe_tracking = true;
        return;
    }

    if (code != LV_EVENT_RELEASED || !s_ui.swipe_tracking || !s_ui.callbacks.on_gallery_nav) {
        return;
    }

    s_ui.swipe_tracking = false;
    lv_point_t point = {0};
    lv_indev_get_point(indev, &point);
    const int16_t delta = point.x - s_ui.swipe_start_x;

    if (delta <= -SWIPE_THRESHOLD_PX) {
        s_ui.callbacks.on_gallery_nav(+1, s_ui.callbacks.user_data);
    } else if (delta >= SWIPE_THRESHOLD_PX) {
        s_ui.callbacks.on_gallery_nav(-1, s_ui.callbacks.user_data);
    }
}

static lv_obj_t *create_shutter_button(lv_obj_t *parent)
{
    lv_obj_t *outer = lv_obj_create(parent);
    lv_obj_set_size(outer, SHUTTER_OUTER_SIZE, SHUTTER_OUTER_SIZE);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(outer, 4, 0);
    lv_obj_set_style_border_color(outer, lv_color_white(), 0);
    lv_obj_set_style_pad_all(outer, 0, 0);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(outer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(outer, shutter_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *inner = lv_obj_create(outer);
    lv_obj_set_size(inner, SHUTTER_INNER_SIZE, SHUTTER_INNER_SIZE);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner, lv_color_white(), 0);
    lv_obj_set_style_border_width(inner, 0, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_remove_flag(inner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(inner);

    return outer;
}

static lv_obj_t *create_gallery_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    style_side_button(btn, SIDE_BTN_SIZE);
    lv_obj_add_event_cb(btn, gallery_open_event_cb, LV_EVENT_CLICKED, NULL);

    s_ui.gallery_thumb = lv_image_create(btn);
    lv_obj_set_size(s_ui.gallery_thumb, CAMERA_UI_THUMB_SIZE, CAMERA_UI_THUMB_SIZE);
    lv_obj_set_style_radius(s_ui.gallery_thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_ui.gallery_thumb, true, 0);
    lv_obj_center(s_ui.gallery_thumb);
    lv_obj_add_flag(s_ui.gallery_thumb, LV_OBJ_FLAG_HIDDEN);

    s_ui.gallery_placeholder = lv_label_create(btn);
    style_icon_label(s_ui.gallery_placeholder, &lv_font_montserrat_14);
    lv_label_set_text(s_ui.gallery_placeholder, LV_SYMBOL_IMAGE);
    lv_obj_center(s_ui.gallery_placeholder);

    return btn;
}

static void build_capture_flash_panel(void)
{
    s_ui.capture_flash_panel = lv_obj_create(s_ui.camera_screen);
    lv_obj_set_size(s_ui.capture_flash_panel, SCREEN_W, SCREEN_H);
    lv_obj_align(s_ui.capture_flash_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_ui.capture_flash_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.capture_flash_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.capture_flash_panel, 0, 0);
    lv_obj_set_style_pad_all(s_ui.capture_flash_panel, 0, 0);
    lv_obj_remove_flag(s_ui.capture_flash_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.capture_flash_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.capture_flash_panel, LV_OBJ_FLAG_HIDDEN);
}

static void build_info_panel(lv_obj_t *parent)
{
    s_ui.info_panel = lv_obj_create(parent);
    lv_obj_set_size(s_ui.info_panel, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_ui.info_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.info_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_ui.info_panel, 0, 0);
    lv_obj_set_style_pad_all(s_ui.info_panel, 0, 0);
    lv_obj_remove_flag(s_ui.info_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.info_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_create(s_ui.info_panel);
    lv_obj_set_size(box, 360, 300);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_bg_color(box, COLOR_PANEL, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    style_icon_label(title, &lv_font_montserrat_14);
    lv_obj_set_style_transform_scale(title, 320, 0);
    lv_label_set_text(title, "Button Sub-board");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *body = lv_label_create(box);
    lv_obj_set_width(body, 300);
    lv_obj_set_style_text_color(body, lv_color_hex(0xD0D0D0), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body,
                      "Long press top button: toggle camera\n\n"
                      "Button sub-board:\n"
                      "Top button (KEY2): capture photo\n"
                      "Bottom button (KEY1): enable/disable flash");
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *close_btn = lv_button_create(box);
    lv_obj_set_size(close_btn, 120, 40);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x3A3A3A), 0);
    lv_obj_add_event_cb(close_btn, info_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close_btn);
    style_icon_label(close_label, &lv_font_montserrat_14);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);
}

static void build_camera_screen(void)
{
    s_ui.camera_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_ui.camera_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ui.camera_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.camera_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_ui.camera_screen, 0, 0);
    lv_obj_set_style_border_width(s_ui.camera_screen, 0, 0);

    s_ui.preview_canvas = lv_canvas_create(s_ui.camera_screen);
    lv_obj_set_size(s_ui.preview_canvas, SCREEN_W, SCREEN_H);
    lv_obj_align(s_ui.preview_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_canvas_set_buffer(
        s_ui.preview_canvas, s_ui.preview_buffer,
        s_ui.preview_width, s_ui.preview_height, LV_COLOR_FORMAT_RGB565);

    build_capture_flash_panel();

    lv_obj_t *top_bar = lv_obj_create(s_ui.camera_screen);
    lv_obj_set_size(top_bar, SCREEN_W, UI_INFO_HIT_SIZE);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, UI_INFO_TOP_MARGIN);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_remove_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(top_bar, LV_OBJ_FLAG_CLICKABLE);

    s_ui.orientation_label = lv_label_create(top_bar);
    style_icon_label(s_ui.orientation_label, &lv_font_montserrat_14);
    lv_obj_set_style_bg_color(s_ui.orientation_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ui.orientation_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(s_ui.orientation_label, 10, 0);
    lv_obj_set_style_pad_top(
        s_ui.orientation_label,
        (UI_INFO_HIT_SIZE - lv_font_get_line_height(&lv_font_montserrat_14)) / 2,
        0);
    lv_obj_set_style_pad_bottom(
        s_ui.orientation_label,
        (UI_INFO_HIT_SIZE - lv_font_get_line_height(&lv_font_montserrat_14)) / 2,
        0);
    lv_obj_set_style_radius(s_ui.orientation_label, 8, 0);
    lv_obj_set_height(s_ui.orientation_label, UI_INFO_HIT_SIZE);
    lv_label_set_text(s_ui.orientation_label, "Rear");
    lv_obj_align(s_ui.orientation_label, LV_ALIGN_LEFT_MID, UI_EDGE_MARGIN, 0);

    lv_obj_t *info_btn = create_plain_image_button(
        top_bar, &icon_info, UI_INFO_ICON_SIZE, UI_INFO_HIT_SIZE,
        info_open_cb);
    lv_obj_align(info_btn, LV_ALIGN_RIGHT_MID, -UI_EDGE_MARGIN, 0);

    lv_obj_t *flash_btn = create_plain_image_button(
        top_bar, &icon_flash_off, UI_INFO_ICON_SIZE, UI_INFO_HIT_SIZE,
        flash_toggle_event_cb);
    lv_obj_align(
        flash_btn, LV_ALIGN_RIGHT_MID,
        -(UI_EDGE_MARGIN + UI_INFO_HIT_SIZE + UI_TOP_BTN_GAP),
        0);
    s_ui.flash_btn_img = lv_obj_get_child(flash_btn, 0);

    lv_obj_t *ctrl_bar = lv_obj_create(s_ui.camera_screen);
    lv_obj_set_size(ctrl_bar, SCREEN_W, SHUTTER_OUTER_SIZE);
    lv_obj_align(ctrl_bar, LV_ALIGN_BOTTOM_MID, 0, -UI_BOTTOM_MARGIN);
    lv_obj_set_style_bg_opa(ctrl_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_bar, 0, 0);
    lv_obj_set_style_pad_all(ctrl_bar, 0, 0);
    lv_obj_remove_flag(ctrl_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ctrl_bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *gallery_btn = create_gallery_button(ctrl_bar);
    lv_obj_align(gallery_btn, LV_ALIGN_LEFT_MID, UI_EDGE_MARGIN, 0);

    lv_obj_t *shutter_btn = create_shutter_button(ctrl_bar);
    lv_obj_align(shutter_btn, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *flip_btn = lv_button_create(ctrl_bar);
    style_side_button(flip_btn, SIDE_BTN_SIZE);
    lv_obj_align(flip_btn, LV_ALIGN_RIGHT_MID, -UI_EDGE_MARGIN, 0);
    lv_obj_add_event_cb(flip_btn, preview_flip_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *flip_img = lv_image_create(flip_btn);
    lv_image_set_src(flip_img, &icon_flip);
    lv_obj_set_size(flip_img, UI_ICON_SIZE, UI_ICON_SIZE);
    lv_obj_center(flip_img);

    build_info_panel(s_ui.camera_screen);

    s_ui.camera_off_panel = lv_obj_create(s_ui.camera_screen);
    lv_obj_set_size(s_ui.camera_off_panel, SCREEN_W, SCREEN_H);
    lv_obj_align(s_ui.camera_off_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_ui.camera_off_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.camera_off_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.camera_off_panel, 0, 0);
    lv_obj_set_style_pad_all(s_ui.camera_off_panel, 0, 0);
    lv_obj_remove_flag(s_ui.camera_off_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.camera_off_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *camera_off_label = lv_label_create(s_ui.camera_off_panel);
    lv_obj_set_width(camera_off_label, SCREEN_W - 48);
    lv_obj_set_style_text_color(camera_off_label, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_font(camera_off_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(camera_off_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(camera_off_label, 8, 0);
    lv_label_set_long_mode(camera_off_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(
        camera_off_label,
        "Camera is off.\nLong press the top button to turn on again.");
    lv_obj_center(camera_off_label);
}

static void build_delete_confirm_panel(lv_obj_t *parent)
{
    s_ui.delete_confirm_panel = lv_obj_create(parent);
    lv_obj_set_size(s_ui.delete_confirm_panel, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_ui.delete_confirm_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.delete_confirm_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_ui.delete_confirm_panel, 0, 0);
    lv_obj_set_style_pad_all(s_ui.delete_confirm_panel, 0, 0);
    lv_obj_remove_flag(s_ui.delete_confirm_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.delete_confirm_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_create(s_ui.delete_confirm_panel);
    lv_obj_set_size(box, 320, 168);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_bg_color(box, COLOR_PANEL, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    style_icon_label(title, &lv_font_montserrat_14);
    lv_label_set_text(title, "Delete this photo?");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *hint = lv_label_create(box);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xB0B0B0), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "This action cannot be undone.");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 48);

    lv_obj_t *cancel_btn = lv_button_create(box);
    lv_obj_set_size(cancel_btn, 120, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 20, -16);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x3A3A3A), 0);
    lv_obj_add_event_cb(cancel_btn, delete_confirm_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    style_icon_label(cancel_label, &lv_font_montserrat_14);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);

    lv_obj_t *delete_btn = lv_button_create(box);
    lv_obj_set_size(delete_btn, 120, 40);
    lv_obj_align(delete_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -16);
    lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0xC0392B), 0);
    lv_obj_add_event_cb(delete_btn, delete_confirm_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *delete_label = lv_label_create(delete_btn);
    style_icon_label(delete_label, &lv_font_montserrat_14);
    lv_label_set_text(delete_label, "Delete");
    lv_obj_center(delete_label);
}

static void build_gallery_screen(void)
{
    s_ui.gallery_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_ui.gallery_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ui.gallery_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.gallery_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_ui.gallery_screen, 0, 0);
    lv_obj_set_style_border_width(s_ui.gallery_screen, 0, 0);

    s_ui.gallery_img = lv_image_create(s_ui.gallery_screen);
    lv_obj_set_size(s_ui.gallery_img, SCREEN_W, SCREEN_H);
    lv_obj_align(s_ui.gallery_img, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_inner_align(s_ui.gallery_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(s_ui.gallery_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.gallery_img, gallery_swipe_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.gallery_img, gallery_swipe_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *top_bar = lv_obj_create(s_ui.gallery_screen);
    lv_obj_set_size(top_bar, SCREEN_W, GALLERY_TOP_BAR_H);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top_bar, COLOR_GALLERY_BAR_BG, 0);
    lv_obj_set_style_bg_opa(top_bar, GALLERY_BAR_BG_OPA, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_remove_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.gallery_title = lv_label_create(top_bar);
    lv_obj_set_width(s_ui.gallery_title, SCREEN_W);
    lv_obj_set_style_text_color(s_ui.gallery_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_ui.gallery_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_transform_scale(s_ui.gallery_title, GALLERY_TITLE_FONT_SCALE, 0);
    lv_obj_set_style_transform_pivot_x(s_ui.gallery_title, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_ui.gallery_title, LV_PCT(50), 0);
    lv_label_set_long_mode(s_ui.gallery_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.gallery_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(s_ui.gallery_title, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_ui.gallery_title, "");
    lv_obj_align(s_ui.gallery_title, LV_ALIGN_CENTER, 0, 0);

    s_ui.gallery_back_btn = create_plain_image_button(
        top_bar, &icon_back, GALLERY_BAR_ICON_SIZE, GALLERY_BAR_HIT_SIZE,
        gallery_close_event_cb);
    lv_obj_align(s_ui.gallery_back_btn, LV_ALIGN_LEFT_MID, GALLERY_BAR_SIDE_MARGIN, 0);
    lv_obj_move_foreground(s_ui.gallery_back_btn);

    s_ui.gallery_delete_btn = create_plain_image_button(
        top_bar, &icon_delete, GALLERY_BAR_ICON_SIZE, GALLERY_BAR_HIT_SIZE,
        gallery_delete_event_cb);
    lv_obj_align(s_ui.gallery_delete_btn, LV_ALIGN_RIGHT_MID, -GALLERY_BAR_SIDE_MARGIN, 0);
    lv_obj_move_foreground(s_ui.gallery_delete_btn);

    build_delete_confirm_panel(s_ui.gallery_screen);
}

esp_err_t camera_ui_create(lv_display_t *display, uint16_t *preview_buffer,
                           int preview_width, int preview_height,
                           const camera_ui_callbacks_t *callbacks)
{
    if (!display || !preview_buffer || !callbacks || preview_width <= 0 ||
        preview_height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.preview_buffer = preview_buffer;
    s_ui.preview_width = preview_width;
    s_ui.preview_height = preview_height;
    s_ui.callbacks = *callbacks;

    s_ui.thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_ui.thumb_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_ui.thumb_dsc.header.w = CAMERA_UI_THUMB_SIZE;
    s_ui.thumb_dsc.header.h = CAMERA_UI_THUMB_SIZE;
    s_ui.thumb_dsc.header.stride = CAMERA_UI_THUMB_SIZE * 2;
    s_ui.thumb_dsc.data_size = sizeof(s_ui.thumb_buffer);
    s_ui.thumb_dsc.data = (const uint8_t *)s_ui.thumb_buffer;

    s_ui.gallery_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_ui.gallery_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_ui.gallery_dsc.data = NULL;
    s_ui.gallery_dsc.data_size = 0;

    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "lock LVGL failed");
        return ESP_ERR_INVALID_STATE;
    }

    build_camera_screen();
    build_gallery_screen();
    camera_ui_set_flash_enabled(false);
    camera_ui_clear_thumb();
    lv_screen_load(s_ui.camera_screen);
    bsp_display_unlock();

    ESP_LOGI(TAG, "LVGL camera UI created");
    (void)display;
    return ESP_OK;
}

void camera_ui_set_flash_enabled(bool enabled)
{
    s_ui.flash_enabled = enabled;
    lv_async_call(flash_apply_async, NULL);
}

void camera_ui_set_preview_flip(bool flipped)
{
    s_ui.preview_flip = flipped;
    lv_async_call(orientation_apply_async, NULL);
}

void camera_ui_invalidate_preview(void)
{
    if (!s_ui.preview_canvas) {
        return;
    }

    if (s_preview_refresh_pending) {
        ++s_preview_refresh_stats.coalesced;
        return;
    }

    if (lv_async_call(preview_invalidate_async, NULL) != LV_RESULT_OK) {
        ++s_preview_refresh_stats.async_fail;
        if ((s_preview_refresh_stats.async_fail % 25U) == 1U) {
            ESP_LOGW(TAG, "Schedule preview refresh failed (lv_malloc?)");
        }
        return;
    }

    s_preview_refresh_pending = true;
}

void camera_ui_get_preview_refresh_stats(
    camera_ui_preview_refresh_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    *out_stats = s_preview_refresh_stats;
}

void camera_ui_reset_preview_refresh_stats(void)
{
    memset(&s_preview_refresh_stats, 0, sizeof(s_preview_refresh_stats));
    s_preview_refresh_pending = false;
}

void camera_ui_play_capture_flash(void)
{
    lv_async_call(capture_flash_play_async, NULL);
}

void camera_ui_set_camera_power_on(bool powered_on)
{
    void *user_data = (void *)(uintptr_t)(powered_on ? 1U : 2U);
    (void)lv_async_call_cancel(camera_power_apply_async, (void *)1U);
    (void)lv_async_call_cancel(camera_power_apply_async, (void *)2U);
    (void)lv_async_call(camera_power_apply_async, user_data);
}

void camera_ui_set_thumb(const uint16_t *rgb565, int width, int height)
{
    if (!rgb565 || width <= 0 || height <= 0 || !s_ui.gallery_thumb) {
        return;
    }

    for (int y = 0; y < CAMERA_UI_THUMB_SIZE; ++y) {
        for (int x = 0; x < CAMERA_UI_THUMB_SIZE; ++x) {
            const int src_x = x * width / CAMERA_UI_THUMB_SIZE;
            const int src_y = y * height / CAMERA_UI_THUMB_SIZE;
            s_ui.thumb_buffer[y * CAMERA_UI_THUMB_SIZE + x] =
                rgb565[src_y * width + src_x];
        }
    }
    s_ui.thumb_valid = true;
    lv_async_call(thumb_apply_async, NULL);
}

void camera_ui_clear_thumb(void)
{
    s_ui.thumb_valid = false;
    lv_async_call(thumb_apply_async, NULL);
}

void camera_ui_show_camera(void)
{
    lv_async_call(screen_load_async, s_ui.camera_screen);
}

void camera_ui_show_gallery(void)
{
    lv_async_call(screen_load_async, s_ui.gallery_screen);
}

void camera_ui_set_gallery_image(const uint16_t *rgb565, int width, int height)
{
    if (!rgb565 || width <= 0 || height <= 0) {
        return;
    }

    s_ui.gallery_dsc.header.w = width;
    s_ui.gallery_dsc.header.h = height;
    s_ui.gallery_dsc.header.stride = width * 2;
    s_ui.gallery_dsc.data = (const uint8_t *)rgb565;
    s_ui.gallery_dsc.data_size = (uint32_t)width * (uint32_t)height * 2U;
    lv_async_call(gallery_image_apply_async, NULL);
}

void camera_ui_set_gallery_title(const char *filename)
{
    strlcpy(s_ui.gallery_title_text, filename ? filename : "",
            sizeof(s_ui.gallery_title_text));
    lv_async_call(gallery_title_apply_async, NULL);
}
