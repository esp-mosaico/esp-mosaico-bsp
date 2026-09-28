/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include "sdkconfig.h"
#include "bsp/esp_mosaico.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "voltage_conversion.h"

#define SENSOR_GPIO 48
#define UPDATE_MS 100
#define SAMPLE_COUNT 16
static const char *TAG = "light_adc";
static lv_obj_t *s_voltage, *s_raw, *s_status, *s_bar;

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       int y, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    return obj;
}

static void create_ui(void)
{
    bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    config.enable_touch = false;
    config.rotation = BSP_DISPLAY_ROTATE_270;
    ESP_ERROR_CHECK(bsp_display_start_with_config(&config) ? ESP_OK : ESP_FAIL);
    if (!bsp_display_lock(-1)) {
        ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x091522), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    label(screen, "AMBIENT LIGHT", &lv_font_montserrat_28, 35, 0xf4c95d);
    label(screen, "GPIO48 / ADC1 CH6", &lv_font_montserrat_20, 85, 0x9cabbc);
    s_voltage = label(screen, "--.--- V", &lv_font_montserrat_48, 165, 0xffffff);
    s_raw = label(screen, "RAW: --", &lv_font_montserrat_20, 242, 0x9cabbc);
    s_bar = lv_bar_create(screen);
    lv_obj_set_size(s_bar, 380, 18);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 298);
    lv_bar_set_range(s_bar, 0, 2000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x70d7a6), LV_PART_INDICATOR);
    label(screen, "0 V                       2 V", &lv_font_montserrat_20, 327, 0x9cabbc);
    s_status = label(screen, "Initializing ADC...", &lv_font_montserrat_20, 377, 0xf4c95d);
    label(screen, "100 ms update / 16 samples", &lv_font_montserrat_20, 425, 0x9cabbc);
    bsp_display_unlock();
}

static void update_ui(esp_err_t result, int raw)
{
    if (!bsp_display_lock(20)) {
        return;
    }
    if (result != ESP_OK) {
        lv_label_set_text(s_voltage, "--.--- V");
        lv_label_set_text(s_raw, "RAW: --");
        lv_label_set_text_fmt(s_status, "ADC error: %s", esp_err_to_name(result));
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    } else {
        lv_label_set_text_fmt(s_raw, "RAW: %d", raw);
#if CONFIG_LIGHT_ADC_CALIBRATED
        const int raw_zero = CONFIG_LIGHT_ADC_RAW_AT_ZERO;
        const int raw_two_volts = CONFIG_LIGHT_ADC_RAW_AT_2V;
        int mv;
        if (!light_raw_to_mv(raw, raw_zero, raw_two_volts, &mv)) {
            lv_label_set_text(s_voltage, "--.--- V");
            lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
            lv_label_set_text(s_status, "Invalid calibration points");
        } else {
            const int magnitude = mv < 0 ? -mv : mv;
            lv_label_set_text_fmt(s_voltage, "%s%d.%03d V", mv < 0 ? "-" : "",
                                  magnitude / 1000, magnitude % 1000);
            lv_bar_set_value(s_bar, mv < 0 ? 0 : (mv > 2000 ? 2000 : mv), LV_ANIM_OFF);
            lv_label_set_text(s_status, mv < 0 || mv > 2000 ? "Outside 0-2 V range" : "Two-point calibrated");
        }
#else
        lv_label_set_text(s_status, "Calibration required (see README)");
#endif
    }
    bsp_display_unlock();
}

void app_main(void)
{
    create_ui();
    adc_unit_t unit;
    adc_channel_t channel;
    adc_oneshot_unit_handle_t adc = NULL;
    esp_err_t ret = adc_oneshot_io_to_channel(SENSOR_GPIO, &unit, &channel);
    /* SDK single-ended channel index; schematic names this pad ADC1_CH3_N. */
    if (ret == ESP_OK && (unit != ADC_UNIT_1 || channel != ADC_CHANNEL_6)) {
        ret = ESP_ERR_INVALID_ARG;
    }
    if (ret == ESP_OK) {
        const adc_oneshot_unit_init_cfg_t config = {.unit_id = unit};
        ret = adc_oneshot_new_unit(&config, &adc);
    }
    if (ret == ESP_OK) {
        /* S31 currently exposes only attenuation 0 and native 17-bit samples.
         * This is NOT the voltage range of the older ESP32 0 dB setting. */
        const adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_0,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_oneshot_config_channel(adc, channel, &config);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO48 ADC setup failed: %s", esp_err_to_name(ret));
        update_ui(ret, 0);
        if (adc) adc_oneshot_del_unit(adc);
        return;
    }
    ESP_LOGI(TAG, "GPIO48 ADC1 CH6: 16-sample average, 100 ms update");
#if !CONFIG_LIGHT_ADC_CALIBRATED
    ESP_LOGW(TAG, "No S31 voltage calibration available: configure measured 0 V / 2 V raw values");
#endif
    TickType_t next = xTaskGetTickCount();
    unsigned iteration = 0;
    while (true) {
        int64_t sum = 0;
        for (unsigned i = 0; i < SAMPLE_COUNT; ++i) {
            int raw;
            ret = adc_oneshot_read(adc, channel, &raw);
            if (ret != ESP_OK) break;
            sum += raw;
        }
        const int average = (int)((sum + SAMPLE_COUNT / 2) / SAMPLE_COUNT);
        update_ui(ret, average);
        if (iteration++ % 10 == 0) {
            ESP_LOGI(TAG, "raw=%d status=%s", average, esp_err_to_name(ret));
        }
        xTaskDelayUntil(&next, pdMS_TO_TICKS(UPDATE_MS));
    }
}
