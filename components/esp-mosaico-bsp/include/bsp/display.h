/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_LCD_H_RES                 480
#define BSP_LCD_V_RES                 480
#define BSP_LCD_X_GAP                 0
#define BSP_LCD_Y_GAP                 0

#define BSP_LCD_PIXEL_CLOCK_HZ        (40 * 1000 * 1000)
#define BSP_LCD_DATA_WIDTH            4
#define BSP_LCD_BITS_PER_PIXEL        16

/**
 * @brief Display rotation angle, applied through the CO5300 MADCTL register.
 *
 * Deliberately independent of the LVGL adapter: the adapter never rotates a QSPI panel,
 * so rotation is owned entirely by the panel driver's swap_xy/mirror interface.
 */
typedef enum {
    BSP_DISPLAY_ROTATE_0   = 0,
    BSP_DISPLAY_ROTATE_90  = 90,
    BSP_DISPLAY_ROTATE_180 = 180,
    BSP_DISPLAY_ROTATE_270 = 270,
} bsp_display_rotation_t;

#define BSP_LCD_ROTATION_DEFAULT      BSP_DISPLAY_ROTATE_0

/*
 * Mounting orientation of the CO5300 glass at BSP_DISPLAY_ROTATE_0, expressed as
 * MADCTL swap/mirror flags. Every requested rotation is composed on top of this base,
 * so adjust these three values (not the rotation mapping) if a board revision mounts
 * the panel differently and the image comes up rotated or mirrored at rotation 0.
 */
#define BSP_LCD_BASE_SWAP_XY          false
#define BSP_LCD_BASE_MIRROR_X         false
#define BSP_LCD_BASE_MIRROR_Y         false

/* Same idea for the CST9217 glass, which may be mounted independently of the LCD. */
#define BSP_TOUCH_BASE_SWAP_XY        false
#define BSP_TOUCH_BASE_MIRROR_X       false
#define BSP_TOUCH_BASE_MIRROR_Y       false
#if CONFIG_BSP_CO5300_ENABLE_TE
#define BSP_LCD_TEAR_AVOID_MODE       ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC
#else
#define BSP_LCD_TEAR_AVOID_MODE       ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT
#endif

typedef struct {
    bsp_display_rotation_t rotation;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;
    uint16_t buffer_height;
    uint32_t task_stack_size;
    bool enable_ppa_accel;
    bool enable_touch;
} bsp_display_config_t;

#define BSP_DISPLAY_DEFAULT_CONFIG() {                 \
    .rotation = BSP_LCD_ROTATION_DEFAULT,              \
    .tear_avoid_mode = BSP_LCD_TEAR_AVOID_MODE,        \
    .buffer_height = 0,                                \
    .task_stack_size = 0,                              \
    .enable_ppa_accel = false,                         \
    .enable_touch = true,                              \
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel);
lv_display_t *bsp_display_start(void);
lv_display_t *bsp_display_start_with_config(const bsp_display_config_t *config);
lv_display_t *bsp_display_get(void);
esp_lcd_panel_handle_t bsp_display_get_panel(void);
/** Get the panel IO handle after bsp_display_new() or bsp_display_start(). */
esp_lcd_panel_io_handle_t bsp_display_get_panel_io(void);
esp_err_t bsp_touch_new(bsp_display_rotation_t rotation, esp_lcd_touch_handle_t *ret_touch);
lv_indev_t *bsp_display_get_input_dev(void);

/**
 * @brief Rotate the display and the touch panel at runtime.
 *
 * The CO5300 runs over QSPI, which esp_lvgl_adapter treats as an opaque panel and never
 * rotates in software. Rotation is therefore applied entirely through the panel driver's
 * esp_lcd_panel_swap_xy()/esp_lcd_panel_mirror() interface, which writes MADCTL (0x36),
 * and the CST9217 coordinate transform is updated to match.
 *
 * LVGL rendering is paused while the registers change and the active screen is
 * invalidated afterwards, because the frame buffers still hold pixels laid out for the
 * previous orientation.
 *
 * @param[in] rotation One of the BSP_DISPLAY_ROTATE_* angles
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the display has not been initialized
 *      - ESP_ERR_INVALID_ARG if @p rotation is not a supported angle
 */
esp_err_t bsp_display_set_rotation(bsp_display_rotation_t rotation);

/** Get the rotation currently applied to the panel and touch panel. */
bsp_display_rotation_t bsp_display_get_rotation(void);

/** Set CO5300 brightness through register 0x51 without changing display state. */
esp_err_t bsp_display_brightness_set(int brightness_percent);

/**
 * @brief Turn the CO5300 display on and resume LVGL rendering if it was paused
 *
 * Sends Sleep Out (0x11) when the panel was previously put into Sleep In by
 * `bsp_display_off()`, then Display On (0x29).
 */
esp_err_t bsp_display_on(void);

/**
 * @brief Pause LVGL and put CO5300 into low-power blank state
 *
 * Sequence: Display Off (0x28) then Sleep In (0x10). Use
 * `bsp_display_enter_deep_standby()` before MCU deep sleep for the lowest IC
 * power state.
 */
esp_err_t bsp_display_off(void);

/**
 * @brief Enter CO5300 Deep Standby (DSTBON, 0x4F)
 *
 * Calls `bsp_display_off()` first when needed, then Deep Standby, then
 * releases LCD CS (GPIO50) to high-impedance so a driven CS cannot leak into
 * the panel after VCC_3V3 is cut. Leaving this state requires a RESX pulse
 * and full panel re-init; MCU deep sleep wake is a full reboot, so that path
 * is the intended consumer.
 */
esp_err_t bsp_display_enter_deep_standby(void);

/**
 * @brief Disconnect LCD CS (GPIO50) and leave the pad floating
 *
 * After the last SPI command, the bus still drives CS high. Isolating the
 * pad prevents leakage into the panel when VCC_3V3 is cut or the MCU enters
 * deep sleep. Safe to call more than once; `bsp_power_prepare_sleep()` and
 * `bsp_display_enter_deep_standby()` already call this.
 */
esp_err_t bsp_display_isolate_cs(void);

bool bsp_display_lock(int32_t timeout_ms);
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif
