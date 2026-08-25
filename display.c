/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "bsp/esp32_s31_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "S31-Mosaico-LCD";
#if CONFIG_BSP_CO5300_ENABLE_TE
#define BSP_LCD_TE_STATE_TEXT "enabled"
#else
#define BSP_LCD_TE_STATE_TEXT "disabled"
#endif
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_io_handle_t s_touch_io;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_display;
static lv_indev_t *s_input;
static bsp_display_config_t s_config;
static bool s_config_valid;
static bool s_spi_bus_initialized;
static bool s_display_paused;
static bool s_display_sleeping;
static bool s_display_deep_standby;

static const co5300_lcd_init_cmd_t s_vendor_init[] = {
    {0x11, NULL, 0, 600},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
#if CONFIG_BSP_CO5300_ENABLE_TE
    {0x35, (uint8_t[]){0x00}, 1, 0},
#endif
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, NULL, 0, 600},
};

/*
 * Orientation is tracked as the 2x2 matrix mapping logical axes onto panel axes, so that a
 * mounting base orientation and a requested rotation can be composed before being reduced
 * back to the swap/mirror flags that MADCTL and esp_lcd_touch understand. Applying the two
 * as independent flag sets would let the rotation silently discard the base.
 */
typedef struct {
    int8_t m00;
    int8_t m01;
    int8_t m10;
    int8_t m11;
} orientation_matrix_t;

static orientation_matrix_t matrix_from_flags(bool swap_xy, bool mirror_x, bool mirror_y)
{
    int8_t axis_x_x = 1;
    int8_t axis_x_y = 0;
    int8_t axis_y_x = 0;
    int8_t axis_y_y = 1;

    if (swap_xy) {
        int8_t tmp_x = axis_x_x;
        int8_t tmp_y = axis_x_y;
        axis_x_x = axis_y_x;
        axis_x_y = axis_y_y;
        axis_y_x = tmp_x;
        axis_y_y = tmp_y;
    }
    if (mirror_x) {
        axis_x_x = -axis_x_x;
        axis_x_y = -axis_x_y;
    }
    if (mirror_y) {
        axis_y_x = -axis_y_x;
        axis_y_y = -axis_y_y;
    }
    return (orientation_matrix_t) {
        .m00 = axis_x_x, .m01 = axis_y_x, .m10 = axis_x_y, .m11 = axis_y_y,
    };
}

static orientation_matrix_t matrix_from_rotation(bsp_display_rotation_t rotation)
{
    switch (rotation) {
    case BSP_DISPLAY_ROTATE_90:
        return matrix_from_flags(true, true, false);
    case BSP_DISPLAY_ROTATE_180:
        return matrix_from_flags(false, true, true);
    case BSP_DISPLAY_ROTATE_270:
        return matrix_from_flags(true, false, true);
    case BSP_DISPLAY_ROTATE_0:
    default:
        return matrix_from_flags(false, false, false);
    }
}

static void compose_orientation(bool base_swap_xy, bool base_mirror_x, bool base_mirror_y,
                                bsp_display_rotation_t rotation,
                                bool *swap_xy, bool *mirror_x, bool *mirror_y)
{
    const orientation_matrix_t base = matrix_from_flags(base_swap_xy, base_mirror_x, base_mirror_y);
    const orientation_matrix_t rot = matrix_from_rotation(rotation);
    const orientation_matrix_t desired = {
        .m00 = rot.m00 * base.m00 + rot.m01 * base.m10,
        .m01 = rot.m00 * base.m01 + rot.m01 * base.m11,
        .m10 = rot.m10 * base.m00 + rot.m11 * base.m10,
        .m11 = rot.m10 * base.m01 + rot.m11 * base.m11,
    };

    for (int test_swap = 0; test_swap <= 1; ++test_swap) {
        for (int test_mirror_x = 0; test_mirror_x <= 1; ++test_mirror_x) {
            for (int test_mirror_y = 0; test_mirror_y <= 1; ++test_mirror_y) {
                const orientation_matrix_t candidate = matrix_from_flags(test_swap, test_mirror_x, test_mirror_y);
                if (candidate.m00 == desired.m00 && candidate.m01 == desired.m01 &&
                        candidate.m10 == desired.m10 && candidate.m11 == desired.m11) {
                    *swap_xy = (bool)test_swap;
                    *mirror_x = (bool)test_mirror_x;
                    *mirror_y = (bool)test_mirror_y;
                    return;
                }
            }
        }
    }

    *swap_xy = base_swap_xy;
    *mirror_x = base_mirror_x;
    *mirror_y = base_mirror_y;
}

static bool rotation_is_valid(bsp_display_rotation_t rotation)
{
    return rotation == BSP_DISPLAY_ROTATE_0 || rotation == BSP_DISPLAY_ROTATE_90 ||
           rotation == BSP_DISPLAY_ROTATE_180 || rotation == BSP_DISPLAY_ROTATE_270;
}

static esp_err_t apply_qspi_bus_drive_cap(void)
{
    const gpio_drive_cap_t strength = (gpio_drive_cap_t)CONFIG_BSP_LCD_QSPI_DRIVE_CAP;
    const gpio_num_t pins[] = {
        BSP_LCD_SCL, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
    };

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        ESP_RETURN_ON_ERROR(gpio_set_drive_capability(pins[i], strength), TAG,
                            "set QSPI GPIO%d drive capability failed", (int)pins[i]);
    }
    return ESP_OK;
}

static esp_err_t apply_panel_orientation(bsp_display_rotation_t rotation)
{
    bool swap_xy, mirror_x, mirror_y;
    compose_orientation(BSP_LCD_BASE_SWAP_XY, BSP_LCD_BASE_MIRROR_X, BSP_LCD_BASE_MIRROR_Y,
                        rotation, &swap_xy, &mirror_x, &mirror_y);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, swap_xy), TAG, "set CO5300 axis swap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y), TAG, "set CO5300 mirror failed");
    return ESP_OK;
}

static esp_err_t apply_touch_orientation(bsp_display_rotation_t rotation)
{
    bool swap_xy, mirror_x, mirror_y;
    compose_orientation(BSP_TOUCH_BASE_SWAP_XY, BSP_TOUCH_BASE_MIRROR_X, BSP_TOUCH_BASE_MIRROR_Y,
                        rotation, &swap_xy, &mirror_x, &mirror_y);
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_swap_xy(s_touch, swap_xy), TAG, "set CST9217 axis swap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_x(s_touch, mirror_x), TAG, "set CST9217 X mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_y(s_touch, mirror_y), TAG, "set CST9217 Y mirror failed");
    return ESP_OK;
}

static void round_qspi_area(lv_area_t *area, void *user_data)
{
    (void)user_data;
    if (!area) {
        return;
    }
    const int max_x = BSP_LCD_H_RES - 1;
    int x1 = area->x1 < 0 ? 0 : (area->x1 > max_x ? max_x : area->x1);
    int x2 = area->x2 < 0 ? 0 : (area->x2 > max_x ? max_x : area->x2);
    area->x1 = (x1 / 4) * 4;
    x2 = ((x2 + 4) / 4) * 4 - 1;
    area->x2 = x2 > max_x ? max_x : x2;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(ret_panel, ESP_ERR_INVALID_ARG, TAG, "panel output is null");
    if (s_panel) {
        *ret_panel = s_panel;
        return ESP_OK;
    }
    const bsp_display_config_t default_config = BSP_DISPLAY_DEFAULT_CONFIG();
    const bsp_display_config_t active = config ? *config : default_config;
    ESP_RETURN_ON_FALSE(rotation_is_valid(active.rotation), ESP_ERR_INVALID_ARG, TAG,
                        "unsupported rotation %d", (int)active.rotation);
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 rail failed");

    const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_SCL, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
        BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG,
                        "initialize CO5300 QSPI bus failed");
    s_spi_bus_initialized = true;
    esp_err_t ret = apply_qspi_bus_drive_cap();
    if (ret != ESP_OK) {
        spi_bus_free(BSP_LCD_SPI_HOST);
        s_spi_bus_initialized = false;
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_config.flags.psram_dma_direct = true;
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_config, &s_panel_io);
    if (ret != ESP_OK) {
        spi_bus_free(BSP_LCD_SPI_HOST);
        s_spi_bus_initialized = false;
        return ret;
    }
    const co5300_vendor_config_t vendor_config = {
        .init_cmds = s_vendor_init,
        .init_cmds_size = sizeof(s_vendor_init) / sizeof(s_vendor_init[0]),
        .flags.use_qspi_interface = true,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = (void *)&vendor_config,
    };
    ret = esp_lcd_new_panel_co5300(s_panel_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        spi_bus_free(BSP_LCD_SPI_HOST);
        s_spi_bus_initialized = false;
        return ret;
    }
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_panel), fail, TAG, "reset CO5300 failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_panel), fail, TAG, "initialize CO5300 failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(s_panel, BSP_LCD_X_GAP, BSP_LCD_Y_GAP),
                      fail, TAG, "set CO5300 gap failed");
    ESP_GOTO_ON_ERROR(apply_panel_orientation(active.rotation), fail, TAG, "set CO5300 orientation failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), fail, TAG, "turn on CO5300 failed");
    s_config = active;
    s_config_valid = true;
    *ret_panel = s_panel;
    ESP_LOGI(TAG, "CO5300 initialized: %dx%d CS=%d SCL=%d D0=%d D1=%d D2=%d D3=%d drive=%d TE=%s(GPIO%d) gap=(%d,%d) rotation=%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_CS, BSP_LCD_SCL, BSP_LCD_DATA0,
             BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3, CONFIG_BSP_LCD_QSPI_DRIVE_CAP,
             BSP_LCD_TE_STATE_TEXT, BSP_LCD_TE,
             BSP_LCD_X_GAP, BSP_LCD_Y_GAP, (int)active.rotation);
    return ESP_OK;

fail:
    esp_lcd_panel_del(s_panel);
    s_panel = NULL;
    esp_lcd_panel_io_del(s_panel_io);
    s_panel_io = NULL;
    if (s_spi_bus_initialized) {
        spi_bus_free(BSP_LCD_SPI_HOST);
        s_spi_bus_initialized = false;
    }
    return ret;
}

esp_err_t bsp_touch_new(bsp_display_rotation_t rotation, esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(ret_touch, ESP_ERR_INVALID_ARG, TAG, "touch output is null");
    if (s_touch) {
        *ret_touch = s_touch;
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(rotation_is_valid(rotation), ESP_ERR_INVALID_ARG, TAG,
                        "unsupported rotation %d", (int)rotation);
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 rail failed");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "shared I2C init failed");
    bool swap_xy, mirror_x, mirror_y;
    compose_orientation(BSP_TOUCH_BASE_SWAP_XY, BSP_TOUCH_BASE_MIRROR_X, BSP_TOUCH_BASE_MIRROR_Y,
                        rotation, &swap_xy, &mirror_x, &mirror_y);
    /* esp_lcd_touch mirrors as (max - value), so these bounds are the largest valid coordinate. */
    const esp_lcd_touch_config_t touch_config = {
        .x_max = BSP_LCD_H_RES - 1,
        .y_max = BSP_LCD_V_RES - 1,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = swap_xy, .mirror_x = mirror_x, .mirror_y = mirror_y},
    };
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    io_config.scl_speed_hz = 400000;
    /* Left at 0 the transfer waits forever, and it runs under the LVGL lock. */
    io_config.transaction_timeout_ms = BSP_LCD_TOUCH_I2C_TIMEOUT_MS;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &io_config, &s_touch_io), TAG,
                        "create CST9217 panel IO failed");
    esp_err_t ret = esp_lcd_touch_new_i2c_cst9217(s_touch_io, &touch_config, &s_touch);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = NULL;
        return ret;
    }
    *ret_touch = s_touch;
    ESP_LOGI(TAG, "CST9217 initialized: INT=%d reset=NC shared_I2C=%d rotation=%d",
             BSP_LCD_TOUCH_INT, BSP_I2C_PORT, (int)rotation);
    return ESP_OK;
}

lv_display_t *bsp_display_start_with_config(const bsp_display_config_t *config)
{
    if (s_display) {
        return s_display;
    }
    bsp_display_config_t active = config ? *config : (bsp_display_config_t)BSP_DISPLAY_DEFAULT_CONFIG();
    esp_err_t ret = bsp_display_new(&active, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display init failed (%s)", esp_err_to_name(ret));
        return NULL;
    }
    if (s_config_valid) {
        active = s_config;
    }
    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
        if (active.task_stack_size) {
            adapter.task_stack_size = active.task_stack_size;
        }
        ret = esp_lv_adapter_init(&adapter);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LVGL adapter init failed (%s)", esp_err_to_name(ret));
            return NULL;
        }
    }
    /*
     * The adapter never rotates a PANEL_IF_OTHER display, and a non-zero rotation there only
     * switches its buffer-priming and pipeline-release heuristics onto paths that assume it
     * does. Rotation lives entirely in MADCTL, so the adapter always sees an upright display.
     */
#if CONFIG_BSP_CO5300_ENABLE_TE
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_TE_DEFAULT_CONFIG(
        s_panel, s_panel_io, BSP_LCD_H_RES, BSP_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0, BSP_LCD_TE,
        BSP_LCD_PIXEL_CLOCK_HZ, BSP_LCD_DATA_WIDTH, BSP_LCD_BITS_PER_PIXEL);
    ESP_LOGI(TAG, "TE anti-tearing path enabled: GPIO=%d mode=%d", BSP_LCD_TE, active.tear_avoid_mode);
#else
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
        s_panel, s_panel_io, BSP_LCD_H_RES, BSP_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0);
    if (active.tear_avoid_mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC) {
        ESP_LOGW(TAG, "TE_SYNC requested while CONFIG_BSP_CO5300_ENABLE_TE is disabled; using default mode");
        active.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT;
    }
    ESP_LOGI(TAG, "TE anti-tearing path disabled: using mode=%d", active.tear_avoid_mode);
#endif
    display_config.tear_avoid_mode = active.tear_avoid_mode;
    if (active.buffer_height) {
        display_config.profile.buffer_height = active.buffer_height;
    }
    display_config.profile.enable_ppa_accel = active.enable_ppa_accel;
    s_display = esp_lv_adapter_register_display(&display_config);
    if (!s_display) {
        ESP_LOGE(TAG, "register CO5300 with LVGL adapter failed");
        return NULL;
    }
    ret = esp_lv_adapter_set_area_rounder_cb(s_display, round_qspi_area, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set QSPI area rounder failed (%s)", esp_err_to_name(ret));
        esp_lv_adapter_unregister_display(s_display);
        s_display = NULL;
        return NULL;
    }
    if (active.enable_touch) {
        ret = bsp_touch_new(active.rotation, &s_touch);
        if (ret == ESP_OK) {
            esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_display, s_touch);
            s_input = esp_lv_adapter_register_touch(&touch_config);
        } else {
            ESP_LOGW(TAG, "touch init failed (%s), continuing without touch", esp_err_to_name(ret));
        }
    }
    ret = esp_lv_adapter_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter start failed (%s)", esp_err_to_name(ret));
        esp_lv_adapter_unregister_display(s_display);
        s_display = NULL;
        return NULL;
    }
    return s_display;
}

lv_display_t *bsp_display_start(void) { return bsp_display_start_with_config(NULL); }
lv_display_t *bsp_display_get(void) { return s_display; }
esp_lcd_panel_handle_t bsp_display_get_panel(void) { return s_panel; }
esp_lcd_panel_io_handle_t bsp_display_get_panel_io(void) { return s_panel_io; }
lv_indev_t *bsp_display_get_input_dev(void) { return s_input; }

bsp_display_rotation_t bsp_display_get_rotation(void)
{
    return s_config_valid ? s_config.rotation : BSP_LCD_ROTATION_DEFAULT;
}

esp_err_t bsp_display_set_rotation(bsp_display_rotation_t rotation)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "CO5300 is not initialized");
    ESP_RETURN_ON_FALSE(rotation_is_valid(rotation), ESP_ERR_INVALID_ARG, TAG,
                        "unsupported rotation %d", (int)rotation);
    const bsp_display_rotation_t previous = bsp_display_get_rotation();
    if (rotation == previous) {
        return ESP_OK;
    }

    /* Pausing drains the worker, so no flush can be mid-CASET/RASET/RAMWR while MADCTL changes. */
    bool paused_here = false;
    if (esp_lv_adapter_is_initialized() && !s_display_paused) {
        ESP_RETURN_ON_ERROR(esp_lv_adapter_pause(-1), TAG, "pause LVGL adapter failed");
        paused_here = true;
    }

    esp_err_t ret = apply_panel_orientation(rotation);
    if (ret == ESP_OK && s_touch) {
        ret = apply_touch_orientation(rotation);
    }
    if (ret == ESP_OK) {
        s_config.rotation = rotation;
        s_config_valid = true;
        /* Frame buffers still hold pixels laid out for the previous orientation. */
        if (s_display && esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_obj_t *screen = lv_display_get_screen_active(s_display);
            if (screen) {
                lv_obj_invalidate(screen);
            }
            esp_lv_adapter_unlock();
        }
    } else {
        (void)apply_panel_orientation(previous);
        if (s_touch) {
            (void)apply_touch_orientation(previous);
        }
    }

    if (paused_here) {
        esp_err_t resume_ret = esp_lv_adapter_resume();
        if (resume_ret != ESP_OK) {
            ESP_LOGE(TAG, "resume LVGL adapter failed (%s)", esp_err_to_name(resume_ret));
            if (ret == ESP_OK) {
                ret = resume_ret;
            }
        }
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "set CO5300 rotation to %d failed", (int)rotation);
    ESP_LOGI(TAG, "CO5300 rotation set to %d (touch synchronized)", (int)rotation);
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "CO5300 is not initialized");
    ESP_RETURN_ON_FALSE(brightness_percent >= 0 && brightness_percent <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "brightness must be within 0-100%%");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(s_panel, (uint8_t)brightness_percent),
                        TAG, "set CO5300 brightness failed");
    ESP_LOGI(TAG, "CO5300 brightness set to %d%%", brightness_percent);
    return ESP_OK;
}

esp_err_t bsp_display_on(void)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "CO5300 is not initialized");
    ESP_RETURN_ON_FALSE(!s_display_deep_standby, ESP_ERR_INVALID_STATE, TAG,
                        "CO5300 is in Deep Standby; reset/re-init required");

    if (s_display_sleeping) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_exit_sleep(s_panel), TAG, "CO5300 Sleep Out failed");
        s_display_sleeping = false;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "turn on CO5300 failed");
    if (s_display_paused) {
        ESP_RETURN_ON_ERROR(esp_lv_adapter_resume(), TAG, "resume LVGL adapter failed");
        s_display_paused = false;
    }
    ESP_LOGI(TAG, "CO5300 display on (Sleep Out + Display On)");
    return ESP_OK;
}

esp_err_t bsp_display_off(void)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "CO5300 is not initialized");
    ESP_RETURN_ON_FALSE(!s_display_deep_standby, ESP_ERR_INVALID_STATE, TAG,
                        "CO5300 is already in Deep Standby");

    bool paused_here = false;
    if (esp_lv_adapter_is_initialized() && !s_display_paused) {
        ESP_RETURN_ON_ERROR(esp_lv_adapter_pause(-1), TAG, "pause LVGL adapter failed");
        s_display_paused = true;
        paused_here = true;
    }

    esp_err_t ret = esp_lcd_panel_disp_on_off(s_panel, false);
    if (ret != ESP_OK && paused_here) {
        (void)esp_lv_adapter_resume();
        s_display_paused = false;
        ESP_RETURN_ON_ERROR(ret, TAG, "turn off CO5300 failed");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "turn off CO5300 failed");

    if (!s_display_sleeping) {
        ret = esp_lcd_panel_co5300_enter_sleep(s_panel);
        if (ret != ESP_OK && paused_here) {
            (void)esp_lcd_panel_disp_on_off(s_panel, true);
            (void)esp_lv_adapter_resume();
            s_display_paused = false;
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "CO5300 Sleep In failed");
        s_display_sleeping = true;
    }

    ESP_LOGI(TAG, "CO5300 display off (Display Off + Sleep In); LVGL paused");
    return ESP_OK;
}

esp_err_t bsp_display_isolate_cs(void)
{
    /* SPI still drives CS high after the last transaction. A driven pad leaks
     * into the panel once VCC_3V3 is cut or the MCU enters deep sleep. */
    gpio_reset_pin(BSP_LCD_CS);
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(BSP_LCD_CS),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "isolate LCD CS GPIO%d failed", BSP_LCD_CS);
    ESP_LOGI(TAG, "LCD CS GPIO%d set to floating", BSP_LCD_CS);
    return ESP_OK;
}

esp_err_t bsp_display_enter_deep_standby(void)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "CO5300 is not initialized");
    if (s_display_deep_standby) {
        return bsp_display_isolate_cs();
    }

    ESP_RETURN_ON_ERROR(bsp_display_off(), TAG, "prepare CO5300 Sleep In failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_enter_deep_standby(s_panel), TAG,
                        "CO5300 Deep Standby failed");
    s_display_deep_standby = true;
    ESP_RETURN_ON_ERROR(bsp_display_isolate_cs(), TAG, "isolate LCD CS after Deep Standby failed");
    ESP_LOGI(TAG, "CO5300 entered Deep Standby");
    return ESP_OK;
}
bool bsp_display_lock(int32_t timeout_ms) { return esp_lv_adapter_lock(timeout_ms) == ESP_OK; }
void bsp_display_unlock(void) { esp_lv_adapter_unlock(); }
