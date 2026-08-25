/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp32_s31_mosaico.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "S31-Mosaico";
static i2c_master_bus_handle_t s_i2c_bus;
static bool s_power_initialized;
static bool s_vcc_3v3_on;
static bool s_led_initialized;
static bool s_motor_initialized;
#if CONFIG_BSP_MOTOR_ENABLE_PWM
static bool s_motor_pwm_initialized;
#endif

static esp_err_t configure_output(gpio_num_t pin, int level)
{
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(pin), ESP_ERR_INVALID_ARG, TAG,
                        "invalid output GPIO: %d", pin);
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), TAG, "preset GPIO%d level failed", pin);
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure GPIO%d failed", pin);
    return ESP_OK;
}

static esp_err_t configure_open_drain_output(gpio_num_t pin, int level)
{
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(pin), ESP_ERR_INVALID_ARG, TAG,
                        "invalid open-drain output GPIO: %d", pin);
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), TAG, "preset GPIO%d level failed", pin);
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure open-drain GPIO%d failed", pin);
    return ESP_OK;
}

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t config = {
        .i2c_port = BSP_I2C_PORT,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_i2c_bus), TAG, "create shared I2C bus failed");
    ESP_LOGI(TAG, "Shared I2C initialized: port=%d SDA=%d SCL=%d", BSP_I2C_PORT, BSP_I2C_SDA, BSP_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return s_i2c_bus;
}

esp_err_t bsp_power_init(void)
{
    if (s_power_initialized) {
        return ESP_OK;
    }
    /* Deep-sleep pad holds survive wake-up on ESP32-S31. Program known-safe
     * inactive states before releasing a retained state to avoid a level
     * glitch during boot. */
    ESP_RETURN_ON_ERROR(configure_output(BSP_POWER_VCC_3V3_CTRL, BSP_POWER_VCC_3V3_OFF_LEVEL), TAG,
                        "configure VCC_3V3 power failed");
    ESP_RETURN_ON_ERROR(configure_output(BSP_POWER_CODEC_3V3_CTRL, BSP_POWER_CODEC_3V3_OFF_LEVEL), TAG,
                        "configure codec 3V3 power failed");
    ESP_RETURN_ON_ERROR(configure_open_drain_output(BSP_POWER_SWITCH_GPIO,
                                                    BSP_POWER_SWITCH_RELEASE_LEVEL), TAG,
                        "configure shutdown signal failed");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(BSP_POWER_VCC_3V3_CTRL), TAG,
                        "release VCC_3V3 GPIO hold failed");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(BSP_POWER_CODEC_3V3_CTRL), TAG,
                        "release codec GPIO hold failed");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(BSP_POWER_SWITCH_GPIO), TAG,
                        "release shutdown GPIO hold failed");
    s_vcc_3v3_on = false;
    s_power_initialized = true;
    ESP_LOGI(TAG, "Power controls initialized: VCC_PW GPIO%d=%d CODEC_PW GPIO%d=%d "
                  "PWR_SW GPIO%d=released(open-drain)",
             BSP_POWER_VCC_3V3_CTRL, BSP_POWER_VCC_3V3_OFF_LEVEL,
             BSP_POWER_CODEC_3V3_CTRL, BSP_POWER_CODEC_3V3_OFF_LEVEL,
             BSP_POWER_SWITCH_GPIO);
    return ESP_OK;
}

#if CONFIG_BSP_VCC_3V3_RAMP_MS > 0
#define VCC_3V3_RAMP_MODE    LEDC_LOW_SPEED_MODE
#define VCC_3V3_RAMP_TIMER   LEDC_TIMER_1
#define VCC_3V3_RAMP_CHANNEL LEDC_CHANNEL_1
/* 8-bit keeps the carrier well above the gate RC pole; 10 bit tops out at 78 kHz. */
#define VCC_3V3_RAMP_RES     LEDC_TIMER_8_BIT
#define VCC_3V3_RAMP_FREQ_HZ 100000

/* The gate of the high-side switch follows the average of the enable pin, so a
 * duty sweep opens it gradually and bounds the inrush current. */
static esp_err_t ramp_vcc_3v3_on(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = VCC_3V3_RAMP_MODE,
        .duty_resolution = VCC_3V3_RAMP_RES,
        .timer_num = VCC_3V3_RAMP_TIMER,
        .freq_hz = VCC_3V3_RAMP_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "configure the VCC_3V3 ramp timer failed");

    const uint32_t max_duty = (1U << VCC_3V3_RAMP_RES) - 1U;
    const uint32_t off_duty = BSP_POWER_VCC_3V3_OFF_LEVEL ? max_duty : 0;
    const uint32_t on_duty = BSP_POWER_VCC_3V3_ON_LEVEL ? max_duty : 0;
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_POWER_VCC_3V3_CTRL,
        .speed_mode = VCC_3V3_RAMP_MODE,
        .channel = VCC_3V3_RAMP_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = VCC_3V3_RAMP_TIMER,
        .duty = off_duty,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "configure VCC_3V3 ramp channel failed");

    const esp_err_t installed = ledc_fade_func_install(0);
    esp_err_t ret = installed == ESP_ERR_INVALID_STATE ? ESP_OK : installed;
    if (ret == ESP_OK) {
        ret = ledc_set_fade_with_time(VCC_3V3_RAMP_MODE, VCC_3V3_RAMP_CHANNEL, on_duty,
                                      CONFIG_BSP_VCC_3V3_RAMP_MS);
    }
    if (ret == ESP_OK) {
        ret = ledc_fade_start(VCC_3V3_RAMP_MODE, VCC_3V3_RAMP_CHANNEL, LEDC_FADE_WAIT_DONE);
    }
    if (installed == ESP_OK) {
        ledc_fade_func_uninstall();
    }

    /* Give the pad back to the GPIO matrix so the deep-sleep hold keeps working;
     * a failed ramp leaves the rail off rather than half driven. */
    const int level = ret == ESP_OK ? BSP_POWER_VCC_3V3_ON_LEVEL : BSP_POWER_VCC_3V3_OFF_LEVEL;
    ledc_stop(VCC_3V3_RAMP_MODE, VCC_3V3_RAMP_CHANNEL, level);
    ESP_RETURN_ON_ERROR(configure_output(BSP_POWER_VCC_3V3_CTRL, level), TAG,
                        "restore the VCC_3V3 enable GPIO failed");
    return ret;
}
#endif

esp_err_t bsp_power_set_vcc_3v3(bool on)
{
    ESP_RETURN_ON_ERROR(bsp_power_init(), TAG, "power init failed");
    if (on == s_vcc_3v3_on) {
        return ESP_OK;
    }
#if CONFIG_BSP_VCC_3V3_RAMP_MS > 0
    if (on) {
        ESP_RETURN_ON_ERROR(ramp_vcc_3v3_on(), TAG, "ramp VCC_3V3 power up failed");
        s_vcc_3v3_on = true;
        ESP_LOGI(TAG, "VCC_3V3 power on: VCC_PW GPIO%d ramped to %d over %d ms",
                 BSP_POWER_VCC_3V3_CTRL, BSP_POWER_VCC_3V3_ON_LEVEL, CONFIG_BSP_VCC_3V3_RAMP_MS);
        return ESP_OK;
    }
#endif
    const int level = on ? BSP_POWER_VCC_3V3_ON_LEVEL : BSP_POWER_VCC_3V3_OFF_LEVEL;
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_POWER_VCC_3V3_CTRL, level), TAG, "set VCC_3V3 power failed");
    s_vcc_3v3_on = on;
    ESP_LOGI(TAG, "VCC_3V3 power %s: VCC_PW GPIO%d=%d", on ? "on" : "off",
             BSP_POWER_VCC_3V3_CTRL, level);
    return ESP_OK;
}

esp_err_t bsp_power_set_codec_3v3(bool on)
{
    ESP_RETURN_ON_ERROR(bsp_power_init(), TAG, "power init failed");
    const int level = on ? BSP_POWER_CODEC_3V3_ON_LEVEL : BSP_POWER_CODEC_3V3_OFF_LEVEL;
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_POWER_CODEC_3V3_CTRL, level), TAG, "set codec 3V3 power failed");
    ESP_LOGI(TAG, "Codec 3V3 power %s: CODEC_PW GPIO%d=%d", on ? "on" : "off",
             BSP_POWER_CODEC_3V3_CTRL, level);
    return ESP_OK;
}

esp_err_t bsp_power_set_shutdown(bool shutdown)
{
    ESP_RETURN_ON_ERROR(bsp_power_init(), TAG, "power init failed");
    const int level = shutdown ? BSP_POWER_SWITCH_ASSERT_LEVEL : BSP_POWER_SWITCH_RELEASE_LEVEL;
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_POWER_SWITCH_GPIO, level), TAG,
                        "set shutdown signal failed");
    ESP_LOGI(TAG, "PWR_SW GPIO%d %s", BSP_POWER_SWITCH_GPIO,
             shutdown ? "asserted low" : "released to high impedance");
    return ESP_OK;
}

esp_err_t bsp_power_prepare_sleep(void)
{
    esp_err_t first_error = ESP_OK;

    if (bsp_display_get_panel()) {
        esp_err_t ret = bsp_display_enter_deep_standby();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "prepare CO5300 Deep Standby failed: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
    } else {
        ESP_LOGD(TAG, "CO5300 Deep Standby skipped: display not initialized");
    }

    /* Float CS even when the panel was never initialized: it is still on VCC_3V3. */
    esp_err_t ret = bsp_display_isolate_cs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "isolate LCD CS failed: %s", esp_err_to_name(ret));
        if (first_error == ESP_OK) {
            first_error = ret;
        }
    }

    ret = bsp_nand_flash_enter_power_save();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "prepare SPI NAND power-save failed: %s", esp_err_to_name(ret));
        if (first_error == ESP_OK) {
            first_error = ret;
        }
    }

    if (first_error == ESP_OK) {
        ESP_LOGI(TAG, "Peripherals prepared for sleep (CO5300 DSTBON + LCD CS floating + NAND standby when available)");
    }
    return first_error;
}

void bsp_power_enter_deep_sleep(void)
{
    ESP_ERROR_CHECK(bsp_power_init());
    /* Only talk to CO5300/NAND while VCC_3V3 is still up. Callers that cut the
     * rail first should invoke bsp_power_prepare_sleep() beforehand. */
    if (s_vcc_3v3_on) {
        esp_err_t prep = bsp_power_prepare_sleep();
        if (prep != ESP_OK) {
            ESP_LOGW(TAG, "Sleep prepare returned %s; continuing into deep sleep",
                     esp_err_to_name(prep));
        }
    } else {
        ESP_LOGW(TAG, "VCC_3V3 already off; skipping peripheral sleep prepare");
    }
    ESP_ERROR_CHECK(gpio_hold_en(BSP_POWER_VCC_3V3_CTRL));
    ESP_ERROR_CHECK(gpio_hold_en(BSP_POWER_CODEC_3V3_CTRL));
    ESP_ERROR_CHECK(gpio_hold_en(BSP_POWER_SWITCH_GPIO));
    ESP_LOGI(TAG, "Entering deep sleep; GPIO60/GPIO56/GPIO57 states retained");
    esp_deep_sleep_start();
}

esp_err_t bsp_iot_button_create(button_handle_t buttons[], int *button_count, int array_size)
{
    ESP_RETURN_ON_FALSE(buttons && array_size >= BSP_BUTTON_NUM, ESP_ERR_INVALID_ARG, TAG,
                        "button array is too small");
    const button_config_t common = {0};
    const button_gpio_config_t gpio_configs[BSP_BUTTON_NUM] = {
        [BSP_BUTTON_AI] = {.gpio_num = BSP_BUTTON_AI_GPIO, .active_level = BSP_BUTTON_ACTIVE_LEVEL,
                           .enable_power_save = true, .disable_pull = false},
    };
    for (int i = 0; i < BSP_BUTTON_NUM; ++i) {
        ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&common, &gpio_configs[i], &buttons[i]), TAG,
                            "create button %d failed", i);
    }
    if (button_count) {
        *button_count = BSP_BUTTON_NUM;
    }
    return ESP_OK;
}

esp_err_t bsp_led_init(void)
{
    if (s_led_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(configure_output(BSP_LED_STATUS_GPIO, BSP_LED_OFF_LEVEL), TAG,
                        "initialize status LED failed");
    s_led_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_led_set(bool on)
{
    ESP_RETURN_ON_ERROR(bsp_led_init(), TAG, "LED init failed");
    return gpio_set_level(BSP_LED_STATUS_GPIO, on ? BSP_LED_ON_LEVEL : BSP_LED_OFF_LEVEL);
}

esp_err_t bsp_motor_init(void)
{
    if (s_motor_initialized) {
        return ESP_OK;
    }
#if CONFIG_BSP_MOTOR_ENABLE_PWM
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "configure motor PWM timer failed");
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_MOTOR_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "configure motor PWM channel failed");
    s_motor_pwm_initialized = true;
#else
    ESP_RETURN_ON_ERROR(configure_output(BSP_MOTOR_GPIO, BSP_MOTOR_OFF_LEVEL), TAG,
                        "initialize motor GPIO failed");
#endif
    s_motor_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_motor_set_strength(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(percent <= 100, ESP_ERR_INVALID_ARG, TAG, "motor strength exceeds 100%%");
    ESP_RETURN_ON_ERROR(bsp_motor_init(), TAG, "motor init failed");
#if CONFIG_BSP_MOTOR_ENABLE_PWM
    ESP_RETURN_ON_FALSE(s_motor_pwm_initialized, ESP_ERR_INVALID_STATE, TAG, "motor PWM is not initialized");
    uint32_t duty = ((1U << 10) - 1U) * percent / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG,
                        "set motor PWM duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    return gpio_set_level(BSP_MOTOR_GPIO, percent ? BSP_MOTOR_ON_LEVEL : BSP_MOTOR_OFF_LEVEL);
#endif
}

esp_err_t bsp_motor_set(bool on)
{
    return bsp_motor_set_strength(on ? 100 : 0);
}
