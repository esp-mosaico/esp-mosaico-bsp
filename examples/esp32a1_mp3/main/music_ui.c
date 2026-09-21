/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "music_ui.h"

#include <stdint.h>

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "music_ui";
static lv_obj_t *s_track_number;
static lv_obj_t *s_title;
static lv_obj_t *s_artist;
static lv_obj_t *s_play_icon;
static lv_obj_t *s_status;
static lv_obj_t *s_volume;
static lv_obj_t *s_volume_bar;
static music_ui_command_callback_t s_command_callback;
static void *s_command_user_data;
LV_FONT_DECLARE(loading_font_24);
static lv_obj_t *s_loading_overlay, *s_loading_title, *s_loading_detail, *s_loading_spinner;

static void loading_bar_height(void *object, int32_t height)
{
    lv_obj_set_height((lv_obj_t *)object, height);
}

static void style_panel(lv_obj_t *object, uint32_t color, int32_t radius)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void command_event_callback(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_command_callback || s_loading_overlay) {
        return;
    }
    const music_player_command_t command =
        (music_player_command_t)(uintptr_t)lv_event_get_user_data(event);
    s_command_callback(command, s_command_user_data);
}

static lv_obj_t *make_touch_button(lv_obj_t *parent, int32_t x, int32_t y,
                                   int32_t width, int32_t height,
                                   uint32_t background, int32_t radius,
                                   music_player_command_t command)
{
    lv_obj_t *button = lv_obj_create(parent);
    style_panel(button, background, radius);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(button, background ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, command_event_callback, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)command);
    return button;
}

esp_err_t music_ui_start(music_ui_command_callback_t callback, void *user_data)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    s_command_callback = callback;
    s_command_user_data = user_data;

    bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    /* BSP rotations are clockwise, therefore 270 degrees is 90 degrees CCW. */
    config.rotation = BSP_DISPLAY_ROTATE_270;
    config.enable_touch = true;
    if (!bsp_display_start_with_config(&config)) {
        ESP_LOGE(TAG, "start display failed");
        return ESP_FAIL;
    }
    const bool screen_touch_ready = bsp_display_get_input_dev() != NULL;
    if (!screen_touch_ready) {
        ESP_LOGW(TAG, "LCD touch input is unavailable; Si12T keys remain usable");
    }
    if (!bsp_display_lock(-1)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_screen_active();
    style_panel(screen, 0x071018, 0);

    lv_obj_t *brand = make_label(screen, "MP3 DEMO", &lv_font_montserrat_20, 0xf4c95d);
    lv_obj_set_pos(brand, 24, 18);
    lv_obj_t *subtitle = make_label(screen, "MUSIC PLAYER", &lv_font_montserrat_14, 0x718096);
    lv_obj_set_pos(subtitle, 335, 22);

    lv_obj_t *card = lv_obj_create(screen);
    style_panel(card, 0x101d2b, 28);
    lv_obj_set_size(card, 432, 218);
    lv_obj_set_pos(card, 24, 56);

    lv_obj_t *disc = lv_obj_create(card);
    style_panel(disc, 0x091018, LV_RADIUS_CIRCLE);
    lv_obj_set_size(disc, 172, 172);
    lv_obj_set_pos(disc, 28, 23);
    lv_obj_set_style_border_width(disc, 2, 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(0x24384b), 0);

    lv_obj_t *disc_center = lv_obj_create(disc);
    style_panel(disc_center, 0xe1a83a, LV_RADIUS_CIRCLE);
    lv_obj_set_size(disc_center, 76, 76);
    lv_obj_center(disc_center);
    lv_obj_t *note = make_label(disc_center, LV_SYMBOL_AUDIO,
                                &lv_font_montserrat_28, 0x071018);
    lv_obj_center(note);

    s_track_number = make_label(card, "TRACK -- / --", &lv_font_montserrat_14, 0xe1a83a);
    lv_obj_set_pos(s_track_number, 226, 38);
    s_title = make_label(card, "Waiting for MP3", &lv_font_montserrat_28, 0xf7fafc);
    lv_obj_set_width(s_title, 185);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(s_title, 226, 72);
    s_artist = make_label(card, "MP3 / FLASH", &lv_font_montserrat_16, 0x8294a8);
    lv_obj_set_pos(s_artist, 226, 116);
    s_status = make_label(card, "PAUSED", &lv_font_montserrat_14, 0xf4c95d);
    lv_obj_set_pos(s_status, 226, 158);

    lv_obj_t *controls = lv_obj_create(screen);
    style_panel(controls, 0x0c1722, 24);
    lv_obj_set_size(controls, 432, 112);
    lv_obj_set_pos(controls, 24, 292);

    lv_obj_t *previous_button = make_touch_button(
        controls, 44, 10, 112, 88, 0, 20, MUSIC_PLAYER_PREVIOUS);
    lv_obj_t *previous = make_label(previous_button, LV_SYMBOL_PREV,
                                    &lv_font_montserrat_24, 0xcbd5e0);
    lv_obj_align(previous, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *previous_key = make_label(previous_button, "TK6", &lv_font_montserrat_14, 0x64748b);
    lv_obj_align(previous_key, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_t *play_button = make_touch_button(
        controls, 180, 12, 72, 72, 0xe1a83a, LV_RADIUS_CIRCLE,
        MUSIC_PLAYER_TOGGLE);
    s_play_icon = make_label(play_button, LV_SYMBOL_PLAY,
                             &lv_font_montserrat_24, 0x071018);
    lv_obj_center(s_play_icon);
    lv_obj_t *play_key = make_label(controls, "TK7", &lv_font_montserrat_14, 0x64748b);
    lv_obj_align(play_key, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *next_button = make_touch_button(
        controls, 276, 10, 112, 88, 0, 20, MUSIC_PLAYER_NEXT);
    lv_obj_t *next = make_label(next_button, LV_SYMBOL_NEXT,
                                &lv_font_montserrat_24, 0xcbd5e0);
    lv_obj_align(next, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *next_key = make_label(next_button, "TK8", &lv_font_montserrat_14, 0x64748b);
    lv_obj_align(next_key, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_t *volume_down = make_touch_button(
        screen, 24, 418, 48, 48, 0x101d2b, LV_RADIUS_CIRCLE,
        MUSIC_PLAYER_VOLUME_DOWN);
    lv_obj_t *volume_down_icon = make_label(
        volume_down, LV_SYMBOL_MINUS, &lv_font_montserrat_24, 0xf7fafc);
    lv_obj_center(volume_down_icon);
    lv_obj_t *volume_up = make_touch_button(
        screen, 408, 418, 48, 48, 0x101d2b, LV_RADIUS_CIRCLE,
        MUSIC_PLAYER_VOLUME_UP);
    lv_obj_t *volume_up_icon = make_label(
        volume_up, LV_SYMBOL_PLUS, &lv_font_montserrat_24, 0xf7fafc);
    lv_obj_center(volume_up_icon);

    s_volume = make_label(screen, "TK5   VOLUME 50%   TK9",
                          &lv_font_montserrat_14, 0x94a3b8);
    lv_obj_align(s_volume, LV_ALIGN_BOTTOM_MID, 0, -47);
    s_volume_bar = lv_bar_create(screen);
    lv_obj_set_size(s_volume_bar, 300, 8);
    lv_obj_align(s_volume_bar, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_bar_set_range(s_volume_bar, 0, 100);
    lv_bar_set_value(s_volume_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(0x172636), 0);
    lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(0xe1a83a), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_volume_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(s_volume_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    /* Opaque topmost child: controls are never exposed before resources are ready.
     * LVGL owns the spinner animation; initialization never holds the UI lock. */
    s_loading_overlay=lv_obj_create(screen);
    style_panel(s_loading_overlay,0x071018,0);
    lv_obj_set_size(s_loading_overlay,LV_PCT(100),LV_PCT(100));
    lv_obj_set_pos(s_loading_overlay,0,0);
    lv_obj_add_flag(s_loading_overlay,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *loading_brand=make_label(s_loading_overlay,"MP3 DEMO",&lv_font_montserrat_28,0xf4c95d);
    lv_obj_align(loading_brand,LV_ALIGN_TOP_MID,0,54);
    lv_obj_t *loading_subtitle = make_label(s_loading_overlay, "A1 AUDIO / NAND MUSIC",
                                          &lv_font_montserrat_14, 0x8294a8);
    lv_obj_align(loading_subtitle, LV_ALIGN_TOP_MID, 0, 96);
    s_loading_spinner=lv_spinner_create(s_loading_overlay);
    lv_obj_set_size(s_loading_spinner,140,140);
    lv_obj_align(s_loading_spinner,LV_ALIGN_CENTER,0,-32);
    lv_spinner_set_anim_params(s_loading_spinner,1400,100);
    lv_obj_set_style_arc_width(s_loading_spinner, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_loading_spinner, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_loading_spinner,lv_color_hex(0x24384b),LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_loading_spinner,lv_color_hex(0xf4c95d),LV_PART_INDICATOR);
    /* Decorative equalizer, not playback activity or a fabricated progress %. */
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *bar = lv_obj_create(s_loading_spinner);
        style_panel(bar, i == 2 ? 0xf4c95d : 0x70d7a6, 4);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, 8, 12);
        lv_obj_align(bar, LV_ALIGN_CENTER, (i - 2) * 14, 0);
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, bar);
        lv_anim_set_exec_cb(&animation, loading_bar_height);
        lv_anim_set_values(&animation, 12, 36 + (i % 3) * 8);
        lv_anim_set_duration(&animation, 360 + i * 70);
        lv_anim_set_playback_duration(&animation, 360 + i * 70);
        lv_anim_set_delay(&animation, i * 90);
        lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
        lv_anim_start(&animation);
    }
    s_loading_title=make_label(s_loading_overlay,"资源加载中",&loading_font_24,0xf7fafc);
    lv_obj_align(s_loading_title,LV_ALIGN_CENTER,0,62);
    s_loading_detail=make_label(s_loading_overlay,"Starting...",&lv_font_montserrat_16,0x8294a8);
    lv_obj_set_width(s_loading_detail,420);
    lv_obj_set_style_text_align(s_loading_detail,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(s_loading_detail,LV_ALIGN_CENTER,0,110);
    bsp_display_unlock();
    ESP_LOGI(TAG, "ready: display rotated 90 degrees counter-clockwise, screen touch=%s",
             screen_touch_ready ? "enabled" : "unavailable");
    return ESP_OK;
}

void music_ui_update(const music_player_state_t *state)
{
    if (!state || !s_status || !bsp_display_lock(-1)) {
        return;
    }

    lv_label_set_text_fmt(s_track_number, "TRACK %02u / %02u",
                          (unsigned)(state->track_index + 1U),
                          (unsigned)state->track_count);
    lv_label_set_text(s_title, state->title);
    lv_label_set_text(s_artist, state->artist);
    lv_label_set_text(s_play_icon, state->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(s_status, state->playing ? "PLAYING" : "PAUSED");
    lv_obj_set_style_text_color(s_status,
                                lv_color_hex(state->playing ? 0x70d7a6 : 0xf4c95d), 0);
    lv_label_set_text_fmt(s_volume, "TK5   VOLUME %u%%   TK9", state->volume);
    lv_bar_set_value(s_volume_bar, state->volume, LV_ANIM_ON);
    bsp_display_unlock();
}

void music_ui_set_error(const char *message)
{
    if (!message || !s_status || !bsp_display_lock(-1)) {
        return;
    }

    if(s_loading_overlay) {
        lv_obj_add_flag(s_loading_spinner,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_loading_title,"加载失败");
        lv_obj_set_style_text_color(s_loading_title,lv_color_hex(0xf87171),0);
        lv_label_set_text_fmt(s_loading_detail,"%s\nCheck hardware/files, then restart",message);
    }
    lv_label_set_text(s_status, message);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xf87171), 0);
    lv_label_set_text(s_play_icon, LV_SYMBOL_WARNING);
    bsp_display_unlock();
}

void music_ui_loading_stage(const char *message)
{
    if(!message || !bsp_display_lock(-1))return;
    if(s_loading_overlay)lv_label_set_text(s_loading_detail,message);
    bsp_display_unlock();
}

void music_ui_finish_loading(void)
{
    if(!bsp_display_lock(-1))return;
    if(s_loading_overlay) {
        lv_obj_delete(s_loading_overlay);
        s_loading_overlay=s_loading_title=s_loading_detail=s_loading_spinner=NULL;
    }
    bsp_display_unlock();
}
