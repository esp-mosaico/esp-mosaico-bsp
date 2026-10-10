/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "bsp/subboard.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_matrix_led.h"
#include "matrix_ui.h"
#include "mosaico_module_mgr.h"

#define MATRIX_W                MOSAICO_MATRIX_LED_WIDTH
#define MATRIX_H                MOSAICO_MATRIX_LED_HEIGHT
#define LED_COUNT               (MATRIX_W * MATRIX_H)
#define BRIGHTNESS              128
#define FRAME_DELAY_MS          30
#define EFFECT_DURATION_MS      8000

static const char *TAG = "matrix_rgb";

static led_strip_handle_t s_strip;
static uint8_t s_fb_r[LED_COUNT];
static uint8_t s_fb_g[LED_COUNT];
static uint8_t s_fb_b[LED_COUNT];

/*
 * N-shape wiring (bottom-left = LED0, top-right = LED63):
 *
 *   col:  0  1  2  3  4  5  6  7
 *   y7:   7 15 23 31 39 47 55 63   <- top
 *   y6:   6 14 22 30 38 46 54 62
 *   ...
 *   y0:   0  8 16 24 32 40 48 56   <- bottom
 *
 * index = col * 8 + row, row grows from bottom to top.
 */
static inline int xy_to_index(int x, int y)
{
    if (x < 0 || x >= MATRIX_W || y < 0 || y >= MATRIX_H) {
        return -1;
    }
    return x * MATRIX_H + y;
}

static inline uint8_t scale8(uint8_t v, uint8_t scale)
{
    return (uint8_t)((uint16_t)v * scale / 255);
}

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
    uint8_t region = h / 60;
    uint8_t remainder = (h - (region * 60)) * 255 / 60;
    uint8_t p = scale8(v, 255 - s);
    uint8_t q = scale8(v, 255 - scale8(s, remainder));
    uint8_t t = scale8(v, 255 - scale8(s, 255 - remainder));

    switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

static void fb_clear(void)
{
    memset(s_fb_r, 0, sizeof(s_fb_r));
    memset(s_fb_g, 0, sizeof(s_fb_g));
    memset(s_fb_b, 0, sizeof(s_fb_b));
}

static void fb_set(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    int idx = xy_to_index(x, y);
    if (idx < 0) {
        return;
    }
    s_fb_r[idx] = r;
    s_fb_g[idx] = g;
    s_fb_b[idx] = b;
}

static void fb_add(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    int idx = xy_to_index(x, y);
    if (idx < 0) {
        return;
    }
    uint16_t nr = (uint16_t)s_fb_r[idx] + r;
    uint16_t ng = (uint16_t)s_fb_g[idx] + g;
    uint16_t nb = (uint16_t)s_fb_b[idx] + b;
    s_fb_r[idx] = nr > 255 ? 255 : (uint8_t)nr;
    s_fb_g[idx] = ng > 255 ? 255 : (uint8_t)ng;
    s_fb_b[idx] = nb > 255 ? 255 : (uint8_t)nb;
}

static void fb_show(const char *effect, mosaico_module_mgr_slot_t slot)
{
    for (int i = 0; i < LED_COUNT; i++) {
        /* The right slot mounts the panel 180 degrees from the left slot. */
        const int pixel = slot == MOSAICO_MODULE_MGR_SLOT_RIGHT ? LED_COUNT - 1 - i : i;
        ESP_ERROR_CHECK(mosaico_matrix_led_set_pixel(s_strip, pixel / MATRIX_H, pixel % MATRIX_H,
                                                    scale8(s_fb_r[i], BRIGHTNESS), scale8(s_fb_g[i], BRIGHTNESS), scale8(s_fb_b[i], BRIGHTNESS)));
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
    matrix_ui_update(s_fb_r, s_fb_g, s_fb_b, effect);
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* ---- Effect 1: Plasma rainbow ---- */
static void effect_plasma(uint32_t t)
{
    float ft = t * 0.04f;
    for (int y = 0; y < MATRIX_H; y++) {
        for (int x = 0; x < MATRIX_W; x++) {
            float v = sinf(x * 0.55f + ft);
            v += sinf(y * 0.65f + ft * 1.2f);
            v += sinf((x + y) * 0.45f + ft * 0.8f);
            v += sinf(sqrtf((x - 3.5f) * (x - 3.5f) + (y - 3.5f) * (y - 3.5f)) * 0.8f - ft);
            uint16_t hue = (uint16_t)((v + 4.0f) * 45.0f) % 360;
            uint8_t r, g, b;
            hsv_to_rgb(hue, 255, 220, &r, &g, &b);
            fb_set(x, y, r, g, b);
        }
    }
}

/* ---- Effect 2: Spiral comet ---- */
static void effect_spiral(uint32_t t)
{
    fb_clear();
    float angle = t * 0.18f;
    float radius = 0.4f + 3.2f * (0.5f + 0.5f * sinf(t * 0.05f));

    for (int trail = 0; trail < 18; trail++) {
        float a = angle - trail * 0.35f;
        float rr = radius * (1.0f - trail * 0.04f);
        float fx = 3.5f + cosf(a) * rr;
        float fy = 3.5f + sinf(a) * rr;
        int x = (int)lroundf(fx);
        int y = (int)lroundf(fy);
        uint8_t fade = (uint8_t)(255 - trail * 12);
        uint8_t r, g, b;
        hsv_to_rgb((uint16_t)(t * 4 + trail * 12) % 360, 255, fade, &r, &g, &b);
        fb_add(x, y, r, g, b);
        fb_add(x + 1, y, scale8(r, 80), scale8(g, 80), scale8(b, 80));
        fb_add(x, y + 1, scale8(r, 80), scale8(g, 80), scale8(b, 80));
    }
}

/* ---- Effect 3: Expanding ripples ---- */
static void effect_ripples(uint32_t t)
{
    fb_clear();
    const float cx[3] = {2.0f, 5.5f, 3.5f};
    const float cy[3] = {2.0f, 5.0f, 3.5f};
    const float phase[3] = {0.0f, 2.1f, 4.2f};

    for (int y = 0; y < MATRIX_H; y++) {
        for (int x = 0; x < MATRIX_W; x++) {
            float bright = 0.0f;
            uint16_t hue = 0;
            for (int i = 0; i < 3; i++) {
                float dx = x - cx[i];
                float dy = y - cy[i];
                float dist = sqrtf(dx * dx + dy * dy);
                float wave = sinf(dist * 1.6f - t * 0.18f + phase[i]);
                float envelope = clampf(1.0f - fabsf(wave) * 1.4f, 0.0f, 1.0f);
                if (envelope > bright) {
                    bright = envelope;
                    hue = (uint16_t)(i * 120 + t * 2) % 360;
                }
            }
            uint8_t r, g, b;
            hsv_to_rgb(hue, 255, (uint8_t)(bright * 230), &r, &g, &b);
            fb_set(x, y, r, g, b);
        }
    }
}

/* ---- Effect 4: Meteor rain ---- */
typedef struct {
    float x;
    float y;
    float speed;
    uint16_t hue;
    bool active;
} meteor_t;

static meteor_t s_meteors[8];

static void meteors_init(void)
{
    for (int i = 0; i < 8; i++) {
        s_meteors[i].active = false;
    }
}

static void effect_meteor(uint32_t t)
{
    (void)t;
    fb_clear();

    for (int i = 0; i < 8; i++) {
        if (!s_meteors[i].active && (esp_random() % 18) == 0) {
            s_meteors[i].active = true;
            s_meteors[i].x = esp_random() % MATRIX_W;
            s_meteors[i].y = MATRIX_H + (esp_random() % 4);
            s_meteors[i].speed = 0.25f + (esp_random() % 40) / 100.0f;
            s_meteors[i].hue = esp_random() % 360;
        }

        if (!s_meteors[i].active) {
            continue;
        }

        s_meteors[i].y -= s_meteors[i].speed;
        if (s_meteors[i].y < -3.0f) {
            s_meteors[i].active = false;
            continue;
        }

        for (int trail = 0; trail < 4; trail++) {
            int x = (int)s_meteors[i].x;
            int y = (int)lroundf(s_meteors[i].y + trail);
            uint8_t fade = (uint8_t)(220 - trail * 55);
            uint8_t r, g, b;
            hsv_to_rgb(s_meteors[i].hue, 200, fade, &r, &g, &b);
            fb_add(x, y, r, g, b);
        }
    }
}

/* ---- Effect 5: Fire ---- */
static uint8_t s_heat[MATRIX_W][MATRIX_H + 1];

static void fire_init(void)
{
    memset(s_heat, 0, sizeof(s_heat));
}

static void effect_fire(uint32_t t)
{
    (void)t;
    for (int x = 0; x < MATRIX_W; x++) {
        s_heat[x][0] = 160 + (esp_random() % 96);
    }

    for (int y = MATRIX_H - 1; y >= 1; y--) {
        for (int x = 0; x < MATRIX_W; x++) {
            int left = s_heat[(x + MATRIX_W - 1) % MATRIX_W][y - 1];
            int mid = s_heat[x][y - 1];
            int right = s_heat[(x + 1) % MATRIX_W][y - 1];
            int below = s_heat[x][y > 1 ? y - 2 : 0];
            int cooling = 20 + (esp_random() % 30);
            int value = (left + mid + right + below) / 4;
            value = value > cooling ? value - cooling : 0;
            s_heat[x][y] = (uint8_t)value;
        }
    }

    for (int y = 0; y < MATRIX_H; y++) {
        for (int x = 0; x < MATRIX_W; x++) {
            uint8_t heat = s_heat[x][y];
            uint8_t r, g, b;
            if (heat < 85) {
                r = heat * 3;
                g = 0;
                b = 0;
            } else if (heat < 170) {
                r = 255;
                g = (heat - 85) * 3;
                b = 0;
            } else {
                r = 255;
                g = 255;
                b = (heat - 170) * 3;
            }
            fb_set(x, y, r, g, b);
        }
    }
}

/* ---- Effect 6: Bouncing rainbow balls ---- */
typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    uint16_t hue;
} ball_t;

static ball_t s_balls[4];

static void balls_init(void)
{
    for (int i = 0; i < 4; i++) {
        s_balls[i].x = 1.0f + i * 1.5f;
        s_balls[i].y = 1.0f + (i % 2) * 3.0f;
        s_balls[i].vx = (i % 2 == 0) ? 0.18f : -0.15f;
        s_balls[i].vy = (i % 3 == 0) ? 0.16f : -0.14f;
        s_balls[i].hue = i * 90;
    }
}

static void effect_balls(uint32_t t)
{
    for (int y = 0; y < MATRIX_H; y++) {
        for (int x = 0; x < MATRIX_W; x++) {
            uint8_t r = scale8(s_fb_r[xy_to_index(x, y)], 180);
            uint8_t g = scale8(s_fb_g[xy_to_index(x, y)], 180);
            uint8_t b = scale8(s_fb_b[xy_to_index(x, y)], 180);
            fb_set(x, y, r, g, b);
        }
    }

    for (int i = 0; i < 4; i++) {
        s_balls[i].x += s_balls[i].vx;
        s_balls[i].y += s_balls[i].vy;
        if (s_balls[i].x < 0.5f || s_balls[i].x > MATRIX_W - 1.5f) {
            s_balls[i].vx = -s_balls[i].vx;
            s_balls[i].x = clampf(s_balls[i].x, 0.5f, MATRIX_W - 1.5f);
        }
        if (s_balls[i].y < 0.5f || s_balls[i].y > MATRIX_H - 1.5f) {
            s_balls[i].vy = -s_balls[i].vy;
            s_balls[i].y = clampf(s_balls[i].y, 0.5f, MATRIX_H - 1.5f);
        }
        s_balls[i].hue = (s_balls[i].hue + 2) % 360;

        for (int y = 0; y < MATRIX_H; y++) {
            for (int x = 0; x < MATRIX_W; x++) {
                float dx = x - s_balls[i].x;
                float dy = y - s_balls[i].y;
                float dist2 = dx * dx + dy * dy;
                if (dist2 < 4.0f) {
                    float intensity = clampf(1.0f - dist2 / 4.0f, 0.0f, 1.0f);
                    uint8_t r, g, b;
                    hsv_to_rgb((s_balls[i].hue + (uint16_t)(t % 360)) % 360, 255,
                               (uint8_t)(intensity * 255), &r, &g, &b);
                    fb_add(x, y, r, g, b);
                }
            }
        }
    }
}

/* ---- Effect 7: Sweeping scan lines ---- */
static void effect_scan(uint32_t t)
{
    fb_clear();
    int col = (t / 3) % (MATRIX_W * 2);
    bool reverse = col >= MATRIX_W;
    int x = reverse ? (MATRIX_W * 2 - 1 - col) : col;
    uint16_t hue = (t * 3) % 360;

    for (int trail = 0; trail < MATRIX_W; trail++) {
        int xx = reverse ? (x + trail) : (x - trail);
        if (xx < 0 || xx >= MATRIX_W) {
            continue;
        }
        uint8_t fade = (uint8_t)(255 - trail * 28);
        uint8_t r, g, b;
        hsv_to_rgb((hue + trail * 8) % 360, 255, fade, &r, &g, &b);
        for (int y = 0; y < MATRIX_H; y++) {
            float edge = 1.0f - fabsf(y - 3.5f) / 4.5f;
            fb_set(xx, y, scale8(r, (uint8_t)(edge * 255)),
                   scale8(g, (uint8_t)(edge * 255)),
                   scale8(b, (uint8_t)(edge * 255)));
        }
    }
}

/* ---- Effect 8: Twinkling stars ---- */
static void effect_twinkle(uint32_t t)
{
    for (int i = 0; i < LED_COUNT; i++) {
        s_fb_r[i] = scale8(s_fb_r[i], 220);
        s_fb_g[i] = scale8(s_fb_g[i], 220);
        s_fb_b[i] = scale8(s_fb_b[i], 220);
    }

    if ((t % 2) == 0) {
        int n = 1 + (esp_random() % 3);
        for (int i = 0; i < n; i++) {
            int x = esp_random() % MATRIX_W;
            int y = esp_random() % MATRIX_H;
            uint8_t r, g, b;
            hsv_to_rgb(esp_random() % 360, 80 + esp_random() % 100, 255, &r, &g, &b);
            fb_set(x, y, r, g, b);
        }
    }
}

typedef void (*effect_fn_t)(uint32_t t);
typedef void (*effect_init_fn_t)(void);

typedef struct {
    const char *name;
    effect_fn_t run;
    effect_init_fn_t init;
} effect_t;

static const effect_t s_effects[] = {
    {"plasma", effect_plasma, NULL},
    {"spiral", effect_spiral, NULL},
    {"ripples", effect_ripples, NULL},
    {"meteor", effect_meteor, meteors_init},
    {"fire", effect_fire, fire_init},
    {"balls", effect_balls, balls_init},
    {"scan", effect_scan, NULL},
    {"twinkle", effect_twinkle, NULL},
};

static bool matrix_present(mosaico_module_mgr_slot_t slot)
{
    mosaico_module_mgr_info_t info;
    ESP_ERROR_CHECK(mosaico_module_mgr_get_info(slot, &info));
    return info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
           info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
           info.eeprom.board_type == MOSAICO_BOARD_TYPE_MATRIX_LED;
}

static void matrix_play(mosaico_module_mgr_slot_t slot)
{
    const int effect_count = sizeof(s_effects) / sizeof(s_effects[0]);
    uint32_t frame = 0;

    while (matrix_present(slot)) {
        for (int e = 0; e < effect_count; e++) {
            ESP_LOGI(TAG, "Playing effect: %s", s_effects[e].name);
            fb_clear();
            if (s_effects[e].init) {
                s_effects[e].init();
            }

            TickType_t start = xTaskGetTickCount();
            while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(EFFECT_DURATION_MS)) {
                if (!matrix_present(slot)) {
                    return;
                }
                s_effects[e].run(frame++);
                fb_show(s_effects[e].name, slot);
                vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
            }
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(matrix_ui_init());
    /* The manager validates EEPROM descriptors and owns board-specific discovery. */
    ESP_ERROR_CHECK(mosaico_module_mgr_init(NULL));
    const mosaico_module_mgr_claim_config_t claim = {
        .expected_type = MOSAICO_BOARD_TYPE_MATRIX_LED,
        .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,
        .timeout_ms = 1000,
    };

    while (true) {
        matrix_ui_set_slot(BSP_SUBBOARD_SLOT_COUNT);
        ESP_LOGI(TAG, "Waiting for a matrix LED board in either slot");
        mosaico_module_lease_t lease;
        esp_err_t ret;
        do {
            ret = mosaico_module_mgr_claim(&claim, &lease);
        } while (ret == ESP_ERR_TIMEOUT);
        ESP_ERROR_CHECK(ret);

        const bsp_subboard_slot_t slot = (bsp_subboard_slot_t)lease.slot;
        ret = mosaico_matrix_led_new(slot, &s_strip);
        if (ret != ESP_OK) {
            ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
            ESP_ERROR_CHECK(ret);
        }
        ESP_ERROR_CHECK(led_strip_clear(s_strip));
        matrix_ui_set_slot(slot);
        matrix_play(lease.slot);

        ESP_LOGI(TAG, "Matrix board removed from %s slot", mosaico_module_mgr_slot_to_name(lease.slot));
        ESP_ERROR_CHECK(led_strip_del(s_strip));
        s_strip = NULL;
        ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
    }
}
