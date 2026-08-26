/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pong_input.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mosaico_joystick.h"

#define REDISCOVERY_INTERVAL_MS 1000U
#define FILTER_FAST_THRESHOLD   0.25f
#define FILTER_FAST_ALPHA       0.75f
#define FILTER_SLOW_ALPHA       0.42f

static const char *TAG = "pong_input";

struct pong_input_context_t {
    mosaico_joystick_handle_t joystick;
    uint32_t sequence;
    uint32_t last_discovery_ms;
    uint16_t buttons;
    uint16_t pressed_edges;
    float filtered_x;
    float filtered_y;
    bool filter_ready;
};

static float filter_axis(float previous, float sample)
{
    if (sample == 0.0f) {
        return 0.0f;
    }
    const float alpha = fabsf(sample - previous) >= FILTER_FAST_THRESHOLD ?
                        FILTER_FAST_ALPHA : FILTER_SLOW_ALPHA;
    return previous + (sample - previous) * alpha;
}

static uint16_t pack_buttons(const mosaico_joystick_data_t *data)
{
    uint16_t buttons = 0;
    for (unsigned i = 0; i < MOSAICO_JOYSTICK_BUTTON_COUNT; ++i) {
        if (data->buttons[i]) {
            buttons |= (uint16_t)(1U << i);
        }
    }
    return buttons;
}

static void disconnect_joystick(struct pong_input_context_t *ctx)
{
    if (ctx->joystick != NULL) {
        mosaico_joystick_del(ctx->joystick);
        ctx->joystick = NULL;
    }
    ctx->buttons = 0;
    ctx->pressed_edges = 0;
    ctx->filtered_x = 0.0f;
    ctx->filtered_y = 0.0f;
    ctx->filter_ready = false;
}

esp_err_t pong_input_create(pong_input_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct pong_input_context_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *out_handle = ctx;
    return ESP_OK;
}

void pong_input_destroy(pong_input_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    disconnect_joystick(handle);
    free(handle);
}

esp_err_t pong_input_poll(pong_input_handle_t handle, uint32_t now_ms,
                          pong_input_t *out_input, bool *out_ready)
{
    if (handle == NULL || out_input == NULL || out_ready == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_input, 0, sizeof(*out_input));
    out_input->sequence = ++handle->sequence;
    out_input->sampled_ms = now_ms;
    *out_ready = false;

    if (handle->joystick == NULL) {
        if ((now_ms - handle->last_discovery_ms) < REDISCOVERY_INTERVAL_MS) {
            return ESP_ERR_NOT_FOUND;
        }
        handle->last_discovery_ms = now_ms;
        mosaico_joystick_config_t config = MOSAICO_JOYSTICK_DEFAULT_CONFIG();
        config.discovery_timeout_ms = 80;
        config.deadzone = 0.06f;
        esp_err_t err = mosaico_joystick_new(&config, &handle->joystick);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "Joystick connected");
    }

    mosaico_joystick_data_t data;
    esp_err_t err = mosaico_joystick_read(handle->joystick, &data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Joystick disconnected: %s", esp_err_to_name(err));
        disconnect_joystick(handle);
        return err;
    }

    const uint16_t buttons = pack_buttons(&data);
    handle->pressed_edges = buttons & (uint16_t)~handle->buttons;
    handle->buttons = buttons;
    if (handle->pressed_edges != 0) {
        ESP_LOGI(TAG, "Button pressed: mask=0x%02x",
                 (unsigned)handle->pressed_edges);
    }
    if (data.state != MOSAICO_JOYSTICK_READY) {
        handle->filter_ready = false;
        return ESP_OK;
    }

    if (!handle->filter_ready) {
        handle->filtered_x = data.x;
        handle->filtered_y = data.y;
        handle->filter_ready = true;
    } else {
        handle->filtered_x = filter_axis(handle->filtered_x, data.x);
        handle->filtered_y = filter_axis(handle->filtered_y, data.y);
    }

    out_input->axis_x = handle->filtered_x;
    /*
     * Joystick positive Y points upward, while the LVGL/world Y axis grows
     * downward. Match joystick_test, which renders normalized Y as -data.y.
     */
    out_input->axis_y = -handle->filtered_y;
    out_input->buttons = buttons;
    *out_ready = true;
    return ESP_OK;
}

uint16_t pong_input_pressed_edges(pong_input_handle_t handle)
{
    if (handle == NULL) {
        return 0;
    }
    const uint16_t edges = handle->pressed_edges;
    handle->pressed_edges = 0;
    return edges;
}
