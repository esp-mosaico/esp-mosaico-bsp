/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "thermal_ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "thermal_share.h"

#define UI_REFRESH_MS       40
#define VIEW_SCALE          15
#define VIEW_W              (THERMAL_W * VIEW_SCALE)   /* 480 */
#define VIEW_H              (THERMAL_H * VIEW_SCALE)   /* 360 */
#define VIEW_BYTES          (VIEW_W * VIEW_H * sizeof(uint16_t))
#define TITLE_H             56
#define STATS_BAR_H         64
#define SAFE_MARGIN         28   /* keep clear of round-corner bezel */
#define MIN_TEMP_SPAN_C     3.0f
#define RANGE_EMA           0.25f

static const char *TAG = "thermal_ui";

static uint16_t s_lut565[256];
static uint16_t *s_rgb565;
static lv_image_dsc_t s_img_dsc;
static lv_obj_t *s_img;
static lv_obj_t *s_lbl_stats;
static lv_obj_t *s_lbl_frame;
static uint32_t s_last_frame_id;
static float s_lo = 20.0f;
static float s_hi = 35.0f;
static bool s_range_ready;

static void build_lut(void)
{
    /* Smooth thermal palette: deep blue -> cyan -> green -> yellow -> orange -> red */
    static const float stops[][4] = {
        {0.00f,   1,   7,  79},
        {0.18f,   0,  60, 180},
        {0.35f,   0, 170, 220},
        {0.50f,  40, 210,  40},
        {0.65f, 230, 230,   0},
        {0.80f, 250, 120,   0},
        {0.92f, 230,  30,  10},
        {1.00f, 180,   0,   0},
    };
    const int n = (int)(sizeof(stops) / sizeof(stops[0]));

    for (int i = 0; i < 256; i++) {
        const float x = (float)i / 255.0f;
        int s = 0;
        while (s < n - 2 && x > stops[s + 1][0]) {
            s++;
        }
        const float *a = stops[s];
        const float *b = stops[s + 1];
        const float den = b[0] - a[0];
        const float u = den > 0.0f ? (x - a[0]) / den : 0.0f;
        const uint8_t r = (uint8_t)(a[1] + (b[1] - a[1]) * u + 0.5f);
        const uint8_t g = (uint8_t)(a[2] + (b[2] - a[2]) * u + 0.5f);
        const uint8_t b8 = (uint8_t)(a[3] + (b[3] - a[3]) * u + 0.5f);
        s_lut565[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b8 >> 3));
    }
}

static int float_cmp(const void *a, const void *b)
{
    const float fa = *(const float *)a;
    const float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void median3x3(const float *src, float *dst)
{
    float win[9];
    for (int y = 0; y < THERMAL_H; y++) {
        for (int x = 0; x < THERMAL_W; x++) {
            int k = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = y + dy;
                if (yy < 0) {
                    yy = 0;
                } else if (yy >= THERMAL_H) {
                    yy = THERMAL_H - 1;
                }
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx;
                    if (xx < 0) {
                        xx = 0;
                    } else if (xx >= THERMAL_W) {
                        xx = THERMAL_W - 1;
                    }
                    win[k++] = src[yy * THERMAL_W + xx];
                }
            }
            for (int i = 0; i < 5; i++) {
                int min_i = i;
                for (int j = i + 1; j < 9; j++) {
                    if (win[j] < win[min_i]) {
                        min_i = j;
                    }
                }
                float tmp = win[i];
                win[i] = win[min_i];
                win[min_i] = tmp;
            }
            dst[y * THERMAL_W + x] = win[4];
        }
    }
}

static float sample_bilinear(const float *img, float x, float y)
{
    if (x < 0.0f) {
        x = 0.0f;
    } else if (x > (float)(THERMAL_W - 1)) {
        x = (float)(THERMAL_W - 1);
    }
    if (y < 0.0f) {
        y = 0.0f;
    } else if (y > (float)(THERMAL_H - 1)) {
        y = (float)(THERMAL_H - 1);
    }

    const int x0 = (int)x;
    const int y0 = (int)y;
    const int x1 = (x0 < THERMAL_W - 1) ? (x0 + 1) : x0;
    const int y1 = (y0 < THERMAL_H - 1) ? (y0 + 1) : y0;
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;

    const float v00 = img[y0 * THERMAL_W + x0];
    const float v10 = img[y0 * THERMAL_W + x1];
    const float v01 = img[y1 * THERMAL_W + x0];
    const float v11 = img[y1 * THERMAL_W + x1];
    const float v0 = v00 + (v10 - v00) * fx;
    const float v1 = v01 + (v11 - v01) * fx;
    return v0 + (v1 - v0) * fy;
}

static void update_range(const float *pixels)
{
    float sorted[THERMAL_N];
    memcpy(sorted, pixels, sizeof(sorted));
    qsort(sorted, THERMAL_N, sizeof(float), float_cmp);

    float lo = sorted[(THERMAL_N - 1) * 5 / 100];
    float hi = sorted[(THERMAL_N - 1) * 95 / 100];
    if (hi < lo + MIN_TEMP_SPAN_C) {
        const float mid = 0.5f * (lo + hi);
        lo = mid - 0.5f * MIN_TEMP_SPAN_C;
        hi = mid + 0.5f * MIN_TEMP_SPAN_C;
    }

    if (!s_range_ready) {
        s_lo = lo;
        s_hi = hi;
        s_range_ready = true;
    } else {
        s_lo = (1.0f - RANGE_EMA) * s_lo + RANGE_EMA * lo;
        s_hi = (1.0f - RANGE_EMA) * s_hi + RANGE_EMA * hi;
        if (s_hi < s_lo + MIN_TEMP_SPAN_C) {
            s_hi = s_lo + MIN_TEMP_SPAN_C;
        }
    }
}

static void render_frame(const thermal_frame_t *frame)
{
    float filtered[THERMAL_N];
    median3x3(frame->pixels, filtered);
    update_range(filtered);

    const float inv = 255.0f / (s_hi - s_lo);
    const float x_scale = (float)(THERMAL_W - 1) / (float)(VIEW_W - 1);
    const float y_scale = (float)(THERMAL_H - 1) / (float)(VIEW_H - 1);

    for (int dy = 0; dy < VIEW_H; dy++) {
        /* Combine the sensor mounting rotation with the active slot orientation. */
        const float sy = (float)(frame->rotate_180 ? VIEW_H - 1 - dy : dy) * y_scale;
        uint16_t *row = &s_rgb565[dy * VIEW_W];
        for (int dx = 0; dx < VIEW_W; dx++) {
            const float sx = (float)(frame->rotate_180 ? VIEW_W - 1 - dx : dx) * x_scale;
            const float t = sample_bilinear(filtered, sx, sy);
            int idx = (int)((t - s_lo) * inv + 0.5f);
            if (idx < 0) {
                idx = 0;
            } else if (idx > 255) {
                idx = 255;
            }
            row[dx] = s_lut565[idx];
        }
    }
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    thermal_frame_t frame;
    if (!thermal_share_get(&frame)) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_lbl_stats, "Waiting for sensor...");
        lv_label_set_text(s_lbl_frame, "OFFLINE");
        s_range_ready = false;
        return;
    }
    if (frame.frame_id == s_last_frame_id) {
        return;
    }
    s_last_frame_id = frame.frame_id;

    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    render_frame(&frame);

    lv_image_set_src(s_img, NULL);
    lv_image_set_src(s_img, &s_img_dsc);
    lv_obj_invalidate(s_img);

    char buf[96];
    snprintf(buf, sizeof(buf), "Ta %.1f  min %.1f  max %.1f  ctr %.1f C",
             frame.ta, frame.tmin, frame.tmax, frame.tcenter);
    lv_label_set_text(s_lbl_stats, buf);

    snprintf(buf, sizeof(buf), "frame %" PRIu32, frame.frame_id);
    lv_label_set_text(s_lbl_frame, buf);
}

esp_err_t thermal_ui_start(void)
{
    build_lut();

    s_rgb565 = heap_caps_malloc(VIEW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rgb565) {
        s_rgb565 = malloc(VIEW_BYTES);
    }
    if (!s_rgb565) {
        ESP_LOGE(TAG, "alloc %u-byte RGB565 view buffer failed", (unsigned)VIEW_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_rgb565, 0, VIEW_BYTES);

    s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    s_img_dsc.header.w = VIEW_W;
    s_img_dsc.header.h = VIEW_H;
    s_img_dsc.header.stride = VIEW_W * sizeof(uint16_t);
    s_img_dsc.data_size = VIEW_BYTES;
    s_img_dsc.data = (const uint8_t *)s_rgb565;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x071522), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "THERMAL CAMERA");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x55C2FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    s_img = lv_image_create(screen);
    lv_image_set_src(s_img, &s_img_dsc);
    lv_obj_set_size(s_img, VIEW_W, VIEW_H);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, TITLE_H);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_set_size(bar, BSP_LCD_H_RES, STATS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x102739), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_stats = lv_label_create(bar);
    lv_label_set_text(s_lbl_stats, "Waiting for frames...");
    lv_obj_set_style_text_font(s_lbl_stats, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_stats, lv_color_hex(0xF5F7FA), 0);
    lv_obj_align(s_lbl_stats, LV_ALIGN_TOP_MID, 0, 8);

    s_lbl_frame = lv_label_create(bar);
    lv_label_set_text(s_lbl_frame, "frame --");
    lv_obj_set_style_text_font(s_lbl_frame, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_frame, lv_color_hex(0x90A4B7), 0);
    lv_obj_align(s_lbl_frame, LV_ALIGN_BOTTOM_MID, 0, -8);

    if (!lv_timer_create(ui_timer_cb, UI_REFRESH_MS, NULL)) {
        lv_obj_delete(s_img);
        s_img = NULL;
        free(s_rgb565);
        s_rgb565 = NULL;
        ESP_LOGE(TAG, "Create UI timer failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Thermal UI ready: %dx%d bilinear view on %dx%d",
             VIEW_W, VIEW_H, BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}
