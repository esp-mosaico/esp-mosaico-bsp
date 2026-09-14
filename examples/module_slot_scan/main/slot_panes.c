/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "slot_panes.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/display.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mosaico_module_camera.h"
#include "mosaico_module_interact.h"

static const char *TAG = "slot_panes";

#define CAM_VIEW_W           240
#define CAM_VIEW_H           240
#define CAM_CROP             640
#define CAM_BUFFER_ALIGN     128
#define COLOR_BG             lv_color_hex(0x070a12)
#define COLOR_PANEL          lv_color_hex(0x151b2b)
#define COLOR_TEXT           lv_color_hex(0xf8fafc)
#define COLOR_MUTED          lv_color_hex(0x94a3b8)
#define COLOR_ACCENT         lv_color_hex(0x38bdf8)

static const mosaico_interact_rgb_t s_led_colors[] = {
    {255, 64, 64}, {64, 255, 112}, {64, 128, 255},
    {192, 96, 255}, {255, 245, 160}, {255, 160, 80},
};

struct slot_pane {
    mosaico_module_mgr_slot_t slot;
    lv_obj_t *root;
    lv_obj_t *body;
    lv_obj_t *title;
    volatile bool stop;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    mosaico_camera_handle_t camera;
    mosaico_interact_handle_t interact;
    ppa_client_handle_t ppa;
    uint16_t *cam_rgb;
    size_t cam_rgb_size;
    lv_obj_t *cam_img;
    lv_image_dsc_t cam_dsc;
    lv_obj_t *sensors;
    lv_obj_t *led_btn[MOSAICO_INTERACT_LED_COUNT];
    uint8_t leds;
};

static slot_pane_t s_panes[MOSAICO_MODULE_MGR_SLOT_COUNT];

static void lock_ui(void)
{
    (void)bsp_display_lock(-1);
}

static void unlock_ui(void)
{
    bsp_display_unlock();
}


static lv_obj_t *add_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, lv_pct(100));
    return label;
}

static void clear_body_locked(slot_pane_t *pane)
{
    lv_obj_clean(pane->body);
    pane->cam_img = NULL;
    pane->sensors = NULL;
    memset(pane->led_btn, 0, sizeof(pane->led_btn));
}

static void show_message_locked(slot_pane_t *pane, const char *side, const char *line1, const char *line2)
{
    clear_body_locked(pane);
    lv_label_set_text(pane->title, side);
    lv_obj_t *box = lv_obj_create(pane->body);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);
    add_label(box, line1, &lv_font_montserrat_20, COLOR_TEXT);
    if (line2 && line2[0]) {
        add_label(box, line2, &lv_font_montserrat_14, COLOR_MUTED);
    }
}

static void show_message(slot_pane_t *pane, const char *side, const char *line1, const char *line2)
{
    lock_ui();
    show_message_locked(pane, side, line1, line2);
    unlock_ui();
}

void slot_pane_show_empty(slot_pane_t *pane)
{
    const char *side = pane->slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "LEFT" : "RIGHT";
    const char *addr = pane->slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "0x50" : "0x51";
    show_message(pane, side, "Empty", addr);
}

void slot_pane_show_other(slot_pane_t *pane, const char *name)
{
    const char *side = pane->slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "LEFT" : "RIGHT";
    show_message(pane, side, name ? name : "Module", "No preview");
}

static void stop_worker(slot_pane_t *pane)
{
    if (!pane->task) {
        return;
    }
    pane->stop = true;
    (void)xSemaphoreTake(pane->done, pdMS_TO_TICKS(2000));
    pane->task = NULL;
    pane->stop = false;
}

static uint32_t align_down_even(uint32_t value)
{
    return value & ~1U;
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static esp_err_t camera_convert(slot_pane_t *pane, const mosaico_camera_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(frame && frame->data, ESP_ERR_INVALID_ARG, TAG, "invalid camera frame");
    ESP_RETURN_ON_FALSE(frame->pixel_format == V4L2_PIX_FMT_UYVY, ESP_ERR_NOT_SUPPORTED, TAG,
                        "unsupported camera format");
    ESP_RETURN_ON_FALSE(frame->width >= CAM_CROP && frame->height >= CAM_CROP, ESP_ERR_INVALID_SIZE, TAG,
                        "camera frame is smaller than crop");

    const uint32_t bytes_per_line =
        frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2U;
    ESP_RETURN_ON_ERROR(
        esp_cache_msync((void *)frame->data, frame->size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE),
        TAG, "invalidate camera frame failed");
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(pane->cam_rgb, pane->cam_rgb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M),
        TAG, "clean camera RGB cache failed");

    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = (void *)frame->data,
            .pic_w = bytes_per_line / 2U,
            .pic_h = frame->height,
            .block_w = CAM_CROP,
            .block_h = CAM_CROP,
            .block_offset_x = align_down_even((frame->width - CAM_CROP) / 2U),
            .block_offset_y = align_down_even((frame->height - CAM_CROP) / 2U),
            .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = pane->cam_rgb,
            .buffer_size = pane->cam_rgb_size,
            .pic_w = CAM_VIEW_W,
            .pic_h = CAM_VIEW_H,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = (float)CAM_VIEW_W / (float)CAM_CROP,
        .scale_y = (float)CAM_VIEW_H / (float)CAM_CROP,
        .mirror_y = true,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(pane->ppa, &operation), TAG, "PPA convert failed");
    return esp_cache_msync(pane->cam_rgb, pane->cam_rgb_size,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static void camera_worker(void *arg)
{
    slot_pane_t *pane = arg;
    while (!pane->stop) {
        mosaico_camera_frame_t frame = {0};
        if (mosaico_camera_get_frame(pane->camera, &frame) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        const esp_err_t convert = camera_convert(pane, &frame);
        (void)mosaico_camera_return_frame(pane->camera, &frame);
        if (convert != ESP_OK || !pane->cam_img) {
            continue;
        }
        lock_ui();
        lv_image_set_src(pane->cam_img, &pane->cam_dsc);
        lv_obj_invalidate(pane->cam_img);
        unlock_ui();
    }
    xSemaphoreGive(pane->done);
    vTaskDelete(NULL);
}

esp_err_t slot_pane_start_camera(slot_pane_t *pane)
{
    slot_pane_stop(pane);
    if (pane->slot != MOSAICO_MODULE_MGR_SLOT_LEFT) {
        show_message(pane, "RIGHT", "Camera", "Left slot only");
        return ESP_ERR_NOT_SUPPORTED;
    }

    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.slot = MOSAICO_MODULE_MGR_SLOT_LEFT;
    config.width = 0;
    config.height = 0;
    config.buffer_count = 1;
    ESP_RETURN_ON_ERROR(mosaico_camera_new(&config, &pane->camera), TAG, "create camera failed");
    esp_err_t ret = mosaico_camera_open(pane->camera);
    if (ret == ESP_OK) {
        ret = mosaico_camera_start_stream(pane->camera);
    }
    if (ret != ESP_OK) {
        (void)mosaico_camera_del(pane->camera);
        pane->camera = NULL;
        show_message(pane, "LEFT", "Camera", esp_err_to_name(ret));
        return ret;
    }

    if (!pane->ppa) {
        const ppa_client_config_t ppa_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        ret = ppa_register_client(&ppa_config, &pane->ppa);
    }
    if (ret == ESP_OK && !pane->cam_rgb) {
        pane->cam_rgb_size = align_up(CAM_VIEW_W * CAM_VIEW_H * sizeof(uint16_t), CAM_BUFFER_ALIGN);
        pane->cam_rgb = heap_caps_aligned_calloc(
            CAM_BUFFER_ALIGN, 1, pane->cam_rgb_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        ret = pane->cam_rgb ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK) {
        slot_pane_stop(pane);
        show_message(pane, "LEFT", "Camera", "Preview alloc failed");
        return ret;
    }

    pane->cam_dsc = (lv_image_dsc_t) {
        .header.magic = LV_IMAGE_HEADER_MAGIC,
        .header.cf = LV_COLOR_FORMAT_RGB565,
        .header.w = CAM_VIEW_W,
        .header.h = CAM_VIEW_H,
        .data_size = CAM_VIEW_W * CAM_VIEW_H * sizeof(uint16_t),
        .data = (const uint8_t *)pane->cam_rgb,
    };

    lock_ui();
    clear_body_locked(pane);
    lv_label_set_text(pane->title, "LEFT  Camera");
    pane->cam_img = lv_image_create(pane->body);
    lv_image_set_src(pane->cam_img, &pane->cam_dsc);
    lv_obj_center(pane->cam_img);
    unlock_ui();

    if (xTaskCreate(camera_worker, "cam_pane", 6144, pane, 5, &pane->task) != pdPASS) {
        slot_pane_stop(pane);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void set_led(slot_pane_t *pane, uint8_t index, bool on)
{
    const mosaico_interact_rgb_t color = on ? s_led_colors[index] : (mosaico_interact_rgb_t) {0};
    if (mosaico_interact_led_set(pane->interact, index, color) == ESP_OK) {
        pane->leds = (uint8_t)((pane->leds & ~(1U << index)) | (on ? (1U << index) : 0));
        if (pane->led_btn[index]) {
            lv_obj_set_style_bg_color(pane->led_btn[index],
                                      on ? lv_color_hex(0x38bdf8) : lv_color_hex(0x1f2937), 0);
        }
    }
}

static void led_clicked(lv_event_t *event)
{
    slot_pane_t *pane = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_current_target(event);
    for (uint8_t i = 0; i < MOSAICO_INTERACT_LED_COUNT; ++i) {
        if (pane->led_btn[i] == target) {
            set_led(pane, i, !(pane->leds & (1U << i)));
            break;
        }
    }
}

static void ir_clicked(lv_event_t *event)
{
    slot_pane_t *pane = lv_event_get_user_data(event);
    const esp_err_t ret = mosaico_interact_ir_send_nec(pane->interact, 0x00, 0x10);
    if (pane->sensors) {
        lv_label_set_text(pane->sensors, ret == ESP_OK ? "IR sent" : "IR failed");
    }
}

static void interact_worker(void *arg)
{
    slot_pane_t *pane = arg;
    while (!pane->stop) {
        mosaico_interact_inputs_t inputs = {0};
        if (mosaico_interact_read_inputs(pane->interact, &inputs) == ESP_OK && pane->sensors) {
            char text[96];
            snprintf(text, sizeof(text), "L %s  R %s\nPIR %s  Light %u",
                     inputs.left_pressed ? "ON" : "off",
                     inputs.right_pressed ? "ON" : "off",
                     inputs.motion_detected ? "yes" : "no",
                     (unsigned)inputs.light_level);
            lock_ui();
            lv_label_set_text(pane->sensors, text);
            unlock_ui();
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    xSemaphoreGive(pane->done);
    vTaskDelete(NULL);
}

esp_err_t slot_pane_start_interact(slot_pane_t *pane)
{
    slot_pane_stop(pane);
    mosaico_interact_config_t config = MOSAICO_INTERACT_DEFAULT_CONFIG();
    config.slot = pane->slot;
    config.button_mode = MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
    ESP_RETURN_ON_ERROR(mosaico_interact_open(&config, &pane->interact), TAG, "open interact failed");

    const char *side = pane->slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "LEFT" : "RIGHT";
    lock_ui();
    clear_body_locked(pane);
    lv_label_set_text_fmt(pane->title, "%s  Interact", side);
    lv_obj_t *col = lv_obj_create(pane->body);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_style_pad_row(col, 6, 0);

    pane->sensors = add_label(col, "Waiting...", &lv_font_montserrat_14, COLOR_TEXT);

    lv_obj_t *grid = lv_obj_create(col);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (uint8_t i = 0; i < MOSAICO_INTERACT_LED_COUNT; ++i) {
        lv_obj_t *btn = lv_button_create(grid);
        lv_obj_set_size(btn, 100, 40);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1f2937), 0);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text_fmt(label, "LED %u", (unsigned)(i + 1));
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, led_clicked, LV_EVENT_CLICKED, pane);
        pane->led_btn[i] = btn;
    }

    lv_obj_t *ir = lv_button_create(col);
    lv_obj_set_size(ir, 160, 40);
    lv_obj_set_style_bg_color(ir, COLOR_ACCENT, 0);
    lv_obj_t *ir_label = lv_label_create(ir);
    lv_label_set_text(ir_label, "Send IR");
    lv_obj_set_style_text_color(ir_label, lv_color_hex(0x0f172a), 0);
    lv_obj_center(ir_label);
    lv_obj_add_event_cb(ir, ir_clicked, LV_EVENT_CLICKED, pane);
    unlock_ui();

    pane->leds = 0;
    if (xTaskCreate(interact_worker, "interact_pane", 4096, pane, 4, &pane->task) != pdPASS) {
        slot_pane_stop(pane);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void slot_pane_stop(slot_pane_t *pane)
{
    stop_worker(pane);
    if (pane->camera) {
        (void)mosaico_camera_stop_stream(pane->camera);
        (void)mosaico_camera_close(pane->camera);
        (void)mosaico_camera_del(pane->camera);
        pane->camera = NULL;
    }
    if (pane->interact) {
        (void)mosaico_interact_led_clear(pane->interact);
        (void)mosaico_interact_close(pane->interact);
        pane->interact = NULL;
    }
    pane->leds = 0;
}

static slot_pane_t *create_pane(lv_obj_t *parent, mosaico_module_mgr_slot_t slot, int32_t x)
{
    slot_pane_t *pane = &s_panes[slot];
    memset(pane, 0, sizeof(*pane));
    pane->slot = slot;
    pane->done = xSemaphoreCreateBinary();
    pane->root = lv_obj_create(parent);
    lv_obj_remove_style_all(pane->root);
    lv_obj_set_size(pane->root, SLOT_PANE_WIDTH, SLOT_PANE_HEIGHT);
    lv_obj_set_pos(pane->root, x, 0);
    lv_obj_set_style_bg_color(pane->root, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(pane->root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(pane->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(pane->root, 36, 0);
    lv_obj_set_style_pad_bottom(pane->root, 36, 0);
    lv_obj_set_style_pad_hor(pane->root, 8, 0);

    pane->title = add_label(pane->root, slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "LEFT" : "RIGHT",
                            &lv_font_montserrat_16, COLOR_ACCENT);
    pane->body = lv_obj_create(pane->root);
    lv_obj_remove_style_all(pane->body);
    lv_obj_set_width(pane->body, lv_pct(100));
    lv_obj_set_flex_grow(pane->body, 1);
    lv_obj_set_style_bg_color(pane->body, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(pane->body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pane->body, 12, 0);
    show_message_locked(pane, slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "LEFT" : "RIGHT",
                        "Empty", slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "0x50" : "0x51");
    return pane;
}

void slot_panes_create(lv_obj_t *parent, slot_pane_t **out_left, slot_pane_t **out_right)
{
    lock_ui();
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 2, SLOT_PANE_HEIGHT);
    lv_obj_set_pos(line, SLOT_PANE_WIDTH - 1, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x334155), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    *out_left = create_pane(parent, MOSAICO_MODULE_MGR_SLOT_LEFT, 0);
    *out_right = create_pane(parent, MOSAICO_MODULE_MGR_SLOT_RIGHT, SLOT_PANE_WIDTH);
    unlock_ui();
}
