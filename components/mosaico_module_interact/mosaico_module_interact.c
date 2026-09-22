/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_module_interact.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "led_strip.h"
#include "soc/soc_caps.h"
#include "touch_button.h"
#include "touch_sensor_lowlevel.h"

static const char *TAG = "mosaico_interact";

#define IR_RMT_RESOLUTION_HZ        1000000
#define IR_NEC_SYMBOL_MAX           34
#define LDR_ADC_MAX                 ((1U << SOC_ADC_DIGI_MAX_BITWIDTH) - 1U)
#define TOUCH_BUTTON_THRESHOLD      500U
#if CONFIG_IDF_TARGET_ESP32S31
// Nominal single-ended endpoints from IDF's S31 ADC tests, not voltage calibration.
#define LDR_S31_ZERO_CODE           2196
#define LDR_S31_HIGH_CODE           4393
#endif

#if SOC_ADC_ATTEN_NUM <= 1
#define INTERACT_ADC_ATTEN          ADC_ATTEN_DB_0
#else
#define INTERACT_ADC_ATTEN          ADC_ATTEN_DB_12
#endif

typedef struct {
    gpio_num_t ldr_io;
    gpio_num_t ir_io;
    gpio_num_t key_l_io;
    gpio_num_t key_r_io;
    gpio_num_t pir_io;
    gpio_num_t ws2812_io;
    uint8_t led_count;
    uint8_t eeprom_addr;
} mosaico_interact_hw_config_t;

struct mosaico_interact_t {
    mosaico_interact_config_t config;
    mosaico_module_mgr_slot_t slot;
    mosaico_module_lease_t lease;
    mosaico_interact_hw_config_t hardware;
    mosaico_interact_button_mode_t active_input;
    bool subboard_claimed;
    SemaphoreHandle_t lock;
    led_strip_handle_t strip;
    mosaico_interact_rgb_t colors[MOSAICO_INTERACT_LED_COUNT];
    adc_oneshot_unit_handle_t adc;
    adc_unit_t adc_unit;
    adc_channel_t ldr_channel;
    int ldr_filtered_q8;
    bool ldr_filter_valid;
    rmt_channel_handle_t ir_chan;
    rmt_encoder_handle_t ir_encoder;
    rmt_symbol_word_t ir_symbols[IR_NEC_SYMBOL_MAX];
    button_handle_t touch_l;
    button_handle_t touch_r;
    atomic_bool touch_l_pressed;
    atomic_bool touch_r_pressed;
};

static StaticSemaphore_t s_hw_lock_storage;
static SemaphoreHandle_t s_hw_lock;
static portMUX_TYPE s_hw_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;
static mosaico_interact_handle_t s_touch_handles[MOSAICO_MODULE_MGR_SLOT_COUNT];
static bool s_touch_lowlevel_created;
#define INTERACT_ADC_UNIT_SLOTS 2
static adc_oneshot_unit_handle_t s_adc[INTERACT_ADC_UNIT_SLOTS];
static int s_adc_refs[INTERACT_ADC_UNIT_SLOTS];

static void hw_lock(void)
{
    portENTER_CRITICAL(&s_hw_lock_init_mux);
    if (s_hw_lock == NULL) {
        s_hw_lock = xSemaphoreCreateMutexStatic(&s_hw_lock_storage);
    }
    SemaphoreHandle_t lock = s_hw_lock;
    portEXIT_CRITICAL(&s_hw_lock_init_mux);
    xSemaphoreTake(lock, portMAX_DELAY);
}

static void hw_unlock(void)
{
    if (s_hw_lock) {
        xSemaphoreGive(s_hw_lock);
    }
}

static int adc_unit_index(adc_unit_t unit)
{
    return (unit == ADC_UNIT_1) ? 0 : 1;
}

static uint8_t scale_channel(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * brightness) / 255U);
}

static esp_err_t get_hw_config(bsp_subboard_slot_t slot, mosaico_interact_hw_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config && slot >= BSP_SUBBOARD_SLOT_LEFT && slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid interaction slot");

    bsp_subboard_slot_config_t slot_config = {0};
    ESP_RETURN_ON_ERROR(bsp_subboard_get_slot_config(slot, &slot_config), TAG, "get slot config failed");
    *out_config = (mosaico_interact_hw_config_t) {
        .ldr_io = bsp_subboard_map_gpio(slot, GPIO_NUM_53),
        .ir_io = bsp_subboard_map_gpio(slot, GPIO_NUM_48),
        .key_l_io = bsp_subboard_map_gpio(slot, GPIO_NUM_13),
        .key_r_io = bsp_subboard_map_gpio(slot, GPIO_NUM_12),
        .pir_io = bsp_subboard_map_gpio(slot, GPIO_NUM_4),
        .ws2812_io = bsp_subboard_map_gpio(slot, GPIO_NUM_15),
        .led_count = MOSAICO_INTERACT_LED_COUNT,
        .eeprom_addr = slot_config.eeprom_addr,
    };
    return ESP_OK;
}

static int gpio_to_touch_channel(gpio_num_t io)
{
    if (io < GPIO_NUM_6 || io > GPIO_NUM_19) {
        return -1;
    }
    return (int)io - (int)GPIO_NUM_6;
}

static esp_err_t configure_pir(const mosaico_interact_hw_config_t *hw)
{
    /* gpio_get_level() is always 0 unless the pad is configured as input. */
    ESP_RETURN_ON_ERROR(gpio_reset_pin(hw->pir_io), TAG, "reset PIR GPIO failed");
    const gpio_config_t pir = {
        .pin_bit_mask = BIT64(hw->pir_io),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&pir);
}

static void reset_key_pads(const mosaico_interact_hw_config_t *hw)
{
    (void)gpio_reset_pin(hw->key_l_io);
    (void)gpio_reset_pin(hw->key_r_io);
}

static esp_err_t configure_gpio_keys(const mosaico_interact_hw_config_t *hw)
{
    reset_key_pads(hw);
    const gpio_config_t keys = {
        .pin_bit_mask = BIT64(hw->key_l_io) | BIT64(hw->key_r_io),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&keys), TAG, "configure key GPIOs failed");
    return ESP_OK;
}

static void touch_event_cb(void *button, void *user_data)
{
    atomic_bool *pressed = user_data;
    atomic_store_explicit(pressed, iot_button_get_event(button) == BUTTON_PRESS_DOWN, memory_order_relaxed);
}

static esp_err_t get_touch_channels(mosaico_interact_handle_t handle, uint32_t channels[2])
{
    const int chan_l = gpio_to_touch_channel(handle->hardware.key_l_io);
    const int chan_r = gpio_to_touch_channel(handle->hardware.key_r_io);
    ESP_RETURN_ON_FALSE(chan_l >= 0 && chan_r >= 0, ESP_ERR_NOT_SUPPORTED, TAG, "KEY GPIOs are not touch channels");
    channels[0] = chan_l;
    channels[1] = chan_r;
    return ESP_OK;
}

static void delete_touch_buttons(mosaico_interact_handle_t handle)
{
    if (handle->touch_l) {
        esp_err_t ret = iot_button_delete(handle->touch_l);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Delete KEY_L touch button failed: %s", esp_err_to_name(ret));
        }
        handle->touch_l = NULL;
    }
    if (handle->touch_r) {
        esp_err_t ret = iot_button_delete(handle->touch_r);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Delete KEY_R touch button failed: %s", esp_err_to_name(ret));
        }
        handle->touch_r = NULL;
    }
    atomic_store_explicit(&handle->touch_l_pressed, false, memory_order_relaxed);
    atomic_store_explicit(&handle->touch_r_pressed, false, memory_order_relaxed);
}

static esp_err_t delete_touch_lowlevel(void)
{
    esp_err_t first_error = touch_sensor_lowlevel_stop();
    if (first_error != ESP_OK) {
        ESP_LOGE(TAG, "Stop touch lowlevel failed: %s", esp_err_to_name(first_error));
    }
    esp_err_t ret = touch_sensor_lowlevel_delete();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Delete touch lowlevel failed: %s", esp_err_to_name(ret));
    } else {
        s_touch_lowlevel_created = false;
    }
    return first_error != ESP_OK ? first_error : ret;
}

static esp_err_t create_touch_button(uint32_t channel, atomic_bool *pressed, button_handle_t *button)
{
    const button_config_t button_config = {0};
    const button_touch_config_t touch_config = {
        .touch_channel = channel,
        .channel_threshold = TOUCH_BUTTON_THRESHOLD,
        .skip_lowlevel_init = true,
    };
    ESP_RETURN_ON_ERROR(iot_button_new_touch_button_device(&button_config, &touch_config, button), TAG, "Create touch button failed");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(*button, BUTTON_PRESS_DOWN, NULL, touch_event_cb, pressed), TAG, "Register touch press failed");
    return iot_button_register_cb(*button, BUTTON_PRESS_UP, NULL, touch_event_cb, pressed);
}

static esp_err_t create_touch_buttons(mosaico_interact_handle_t handle)
{
    uint32_t channels[2];
    ESP_RETURN_ON_ERROR(get_touch_channels(handle, channels), TAG, "Get touch channels failed");
    ESP_RETURN_ON_ERROR(create_touch_button(channels[0], &handle->touch_l_pressed, &handle->touch_l), TAG, "Create KEY_L failed");
    ESP_RETURN_ON_ERROR(create_touch_button(channels[1], &handle->touch_r_pressed, &handle->touch_r), TAG, "Create KEY_R failed");
    return ESP_OK;
}

static esp_err_t rebuild_touch_locked(void)
{
    uint32_t channels[MOSAICO_MODULE_MGR_SLOT_COUNT * 2U] = {0};
    uint32_t channel_count = 0;
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        mosaico_interact_handle_t handle = s_touch_handles[i];
        if (!handle) {
            continue;
        }
        delete_touch_buttons(handle);
        uint32_t handle_channels[2];
        ESP_RETURN_ON_ERROR(get_touch_channels(handle, handle_channels), TAG, "Get touch channels failed");
        channels[channel_count++] = handle_channels[0];
        channels[channel_count++] = handle_channels[1];
    }

    if (s_touch_lowlevel_created) {
        ESP_RETURN_ON_ERROR(delete_touch_lowlevel(), TAG, "Reset touch lowlevel failed");
    }
    if (channel_count == 0) {
        return ESP_OK;
    }

    touch_lowlevel_config_t config = {
        .channel_num = channel_count,
        .channel_list = channels,
    };
    ESP_RETURN_ON_ERROR(touch_sensor_lowlevel_create(&config), TAG, "Create touch lowlevel failed");
    s_touch_lowlevel_created = true;

    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if (s_touch_handles[i]) {
            ret = create_touch_buttons(s_touch_handles[i]);
            if (ret != ESP_OK) {
                goto fail;
            }
        }
    }
    ESP_GOTO_ON_ERROR(touch_sensor_lowlevel_start(), fail, TAG, "Start touch lowlevel failed");
    ESP_LOGI(TAG, "Touch lowlevel rebuilt with %" PRIu32 " channels", channel_count);
    return ESP_OK;

fail:
    for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if (s_touch_handles[i]) {
            delete_touch_buttons(s_touch_handles[i]);
        }
    }
    if (s_touch_lowlevel_created) {
        (void)delete_touch_lowlevel();
    }
    return ret;
}

static void stop_touch(mosaico_interact_handle_t handle)
{
    hw_lock();
    const size_t index = handle->slot;
    const bool attached = s_touch_handles[index] == handle;
    if (attached) {
        s_touch_handles[index] = NULL;
    }
    delete_touch_buttons(handle);
    if (attached) {
        esp_err_t ret = rebuild_touch_locked();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Rebuild touch after detach failed: %s", esp_err_to_name(ret));
        }
    }
    hw_unlock();
}

static esp_err_t start_touch(mosaico_interact_handle_t handle)
{
    uint32_t channels[2];
    ESP_RETURN_ON_ERROR(get_touch_channels(handle, channels), TAG, "Get touch channels failed");

    /* GPIO pull-up leftover after Key mode keeps the analog pad biased. */
    reset_key_pads(&handle->hardware);
    vTaskDelay(pdMS_TO_TICKS(20));

    hw_lock();
    const size_t index = handle->slot;
    if (s_touch_handles[index]) {
        hw_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_touch_handles[index] = handle;
    esp_err_t ret = rebuild_touch_locked();
    if (ret != ESP_OK) {
        s_touch_handles[index] = NULL;
        esp_err_t restore_ret = rebuild_touch_locked();
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Restore existing touch buttons failed: %s", esp_err_to_name(restore_ret));
        }
    } else {
        ESP_LOGI(TAG, "Touch buttons on KEY_L=CH%" PRIu32 " KEY_R=CH%" PRIu32, channels[0], channels[1]);
    }
    hw_unlock();
    return ret;
}

static esp_err_t apply_button_mode(mosaico_interact_handle_t handle, mosaico_interact_button_mode_t mode)
{
    const bool allow_fallback = mode == MOSAICO_INTERACT_BUTTON_MODE_AUTO;
    if (allow_fallback) {
        /* Key and finger share the touch pads; no GPIO/touch hard switch. */
        mode = MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
    }

    if (mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH) {
        stop_touch(handle);
        esp_err_t ret = start_touch(handle);
        if (ret != ESP_OK) {
            if (!allow_fallback) {
                ESP_LOGE(TAG, "Touch initialization failed: %s", esp_err_to_name(ret));
                return ret;
            }
            ESP_LOGW(TAG, "Touch unavailable on slot=%s (%s), stay on GPIO keys",
                     mosaico_module_mgr_slot_to_name(handle->slot),
                     esp_err_to_name(ret));
            ESP_RETURN_ON_ERROR(configure_gpio_keys(&handle->hardware), TAG,
                                "restore GPIO keys failed");
            handle->active_input = MOSAICO_INTERACT_BUTTON_MODE_GPIO;
            return ESP_OK;
        }
        handle->active_input = MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
        return ESP_OK;
    }

    stop_touch(handle);
    ESP_RETURN_ON_ERROR(configure_gpio_keys(&handle->hardware), TAG,
                        "configure GPIO keys failed");
    handle->active_input = MOSAICO_INTERACT_BUTTON_MODE_GPIO;
    return ESP_OK;
}

static void read_keys(mosaico_interact_handle_t handle, bool *key_l, bool *key_r)
{
    if (handle->active_input == MOSAICO_INTERACT_BUTTON_MODE_TOUCH) {
        *key_l = atomic_load_explicit(&handle->touch_l_pressed, memory_order_relaxed);
        *key_r = atomic_load_explicit(&handle->touch_r_pressed, memory_order_relaxed);
        return;
    }

    *key_l = gpio_get_level(handle->hardware.key_l_io) == 0;
    *key_r = gpio_get_level(handle->hardware.key_r_io) == 0;
}

static esp_err_t create_led_strip(mosaico_interact_handle_t handle)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = handle->hardware.ws2812_io,
        .max_leds = handle->hardware.led_count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    /* One memory block per strip. 64 symbols on ESP32-S31 (48 words/channel)
     * occupies the neighbour TX slot and exhausts the 4-channel pool when
     * two Interaction boards each also keep an IR TX channel. */
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .flags = {
            .with_dma = false,
        },
    };
    return led_strip_new_rmt_device(&strip_config, &rmt_config, &handle->strip);
}

static esp_err_t push_leds(mosaico_interact_handle_t handle, const mosaico_interact_rgb_t *colors)
{
    if (!handle->strip) {
        return ESP_ERR_NOT_FOUND;
    }
    for (uint8_t i = 0; i < handle->hardware.led_count; ++i) {
        const mosaico_interact_rgb_t color = colors[i];
        ESP_RETURN_ON_ERROR(
            led_strip_set_pixel(handle->strip, i,
                                scale_channel(color.r, handle->config.led_brightness),
                                scale_channel(color.g, handle->config.led_brightness),
                                scale_channel(color.b, handle->config.led_brightness)),
            TAG, "set LED %u failed", i);
    }
    ESP_RETURN_ON_ERROR(led_strip_refresh(handle->strip), TAG, "refresh LEDs failed");
    memcpy(handle->colors, colors, handle->hardware.led_count * sizeof(*colors));
    return ESP_OK;
}

static esp_err_t create_adc(mosaico_interact_handle_t handle)
{
    adc_unit_t unit = ADC_UNIT_1;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(handle->hardware.ldr_io, &unit,
                                                  &handle->ldr_channel),
                        TAG, "LDR GPIO%d is not an ADC channel",
                        handle->hardware.ldr_io);

    const int idx = adc_unit_index(unit);
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = INTERACT_ADC_ATTEN,
    };

    hw_lock();
    const bool created = s_adc[idx] == NULL;
    if (!s_adc[idx]) {
        const adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc[idx]);
        if (ret != ESP_OK) {
            hw_unlock();
            ESP_LOGE(TAG, "create LDR ADC unit failed");
            return ret;
        }
    }
    esp_err_t ret = adc_oneshot_config_channel(s_adc[idx], handle->ldr_channel, &chan_cfg);
    if (ret == ESP_OK) {
        handle->adc = s_adc[idx];
        handle->adc_unit = unit;
        s_adc_refs[idx]++;
    } else if (created) {
        const esp_err_t delete_ret = adc_oneshot_del_unit(s_adc[idx]);
        if (delete_ret == ESP_OK) {
            s_adc[idx] = NULL;
        } else {
            ESP_LOGE(TAG, "Delete ADC unit after channel failure failed: %s", esp_err_to_name(delete_ret));
        }
    }
    hw_unlock();

    return ret;
}

static void release_adc(mosaico_interact_handle_t handle)
{
    if (!handle->adc) {
        return;
    }

    const int idx = adc_unit_index(handle->adc_unit);
    hw_lock();
    if (handle->adc == s_adc[idx]) {
        s_adc_refs[idx]--;
        if (s_adc_refs[idx] <= 0) {
            const esp_err_t ret = adc_oneshot_del_unit(s_adc[idx]);
            if (ret == ESP_OK) {
                s_adc[idx] = NULL;
            } else {
                ESP_LOGE(TAG, "Delete LDR ADC unit failed; handle retained: %s", esp_err_to_name(ret));
            }
            s_adc_refs[idx] = 0;
        }
    } else {
        (void)adc_oneshot_del_unit(handle->adc);
    }
    handle->adc = NULL;
    hw_unlock();
}

static void release_ir(mosaico_interact_handle_t handle)
{
    if (handle->ir_chan) {
        (void)rmt_disable(handle->ir_chan);
        (void)rmt_del_channel(handle->ir_chan);
        handle->ir_chan = NULL;
    }
    if (handle->ir_encoder) {
        (void)rmt_del_encoder(handle->ir_encoder);
        handle->ir_encoder = NULL;
    }
}

static esp_err_t create_ir(mosaico_interact_handle_t handle)
{
    if (handle->ir_chan && handle->ir_encoder) {
        return ESP_OK;
    }
    release_ir(handle);

    const rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = handle->hardware.ir_io,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RMT_RESOLUTION_HZ,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &handle->ir_chan), TAG,
                        "create IR RMT channel failed");

    const rmt_carrier_config_t carrier = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33f,
    };
    esp_err_t ret = rmt_apply_carrier(handle->ir_chan, &carrier);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable IR 38 kHz carrier failed");
        release_ir(handle);
        return ret;
    }

    const rmt_copy_encoder_config_t copy_cfg = {};
    ret = rmt_new_copy_encoder(&copy_cfg, &handle->ir_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create IR copy encoder failed");
        release_ir(handle);
        return ret;
    }
    ret = rmt_enable(handle->ir_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable IR RMT channel failed");
        release_ir(handle);
        return ret;
    }
    return ESP_OK;
}

static void fill_nec_bit(rmt_symbol_word_t *sym, bool one)
{
    sym->level0 = 1;
    sym->duration0 = 560;
    sym->level1 = 0;
    sym->duration1 = one ? 1690 : 560;
}

static size_t build_nec_symbols(uint8_t address, uint8_t command,
                                rmt_symbol_word_t *out)
{
    size_t n = 0;
    out[n++] = (rmt_symbol_word_t) {
        .level0 = 1, .duration0 = 9000, .level1 = 0, .duration1 = 4500,
    };

    const uint16_t words[2] = {
        (uint16_t)(address | ((uint16_t)(~address) << 8)),
        (uint16_t)(command | ((uint16_t)(~command) << 8)),
    };
    for (size_t w = 0; w < 2; ++w) {
        for (int bit = 0; bit < 16; ++bit) {
            fill_nec_bit(&out[n++], (words[w] >> bit) & 0x1);
        }
    }
    out[n++] = (rmt_symbol_word_t) {
        .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560,
    };
    return n;
}

static esp_err_t release_resources(mosaico_interact_handle_t handle)
{
    if (!handle) {
        return ESP_OK;
    }

    stop_touch(handle);

    if (handle->strip) {
        led_strip_clear(handle->strip);
        led_strip_del(handle->strip);
        handle->strip = NULL;
    }
    release_ir(handle);
    release_adc(handle);
    if (handle->subboard_claimed) {
        esp_err_t ret = mosaico_module_mgr_release(&handle->lease);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Release slot %s failed: %s",
                     mosaico_module_mgr_slot_to_name(handle->slot),
                     esp_err_to_name(ret));
            return ret;
        }
        handle->subboard_claimed = false;
        handle->lease = (mosaico_module_lease_t) {0};
    }
    return ESP_OK;
}

esp_err_t mosaico_interact_open(const mosaico_interact_config_t *config, mosaico_interact_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG,
                        "interact output handle is null");
    *out_handle = NULL;

    mosaico_interact_config_t active =
        config ? *config
               : (mosaico_interact_config_t)MOSAICO_INTERACT_DEFAULT_CONFIG();
    ESP_RETURN_ON_FALSE(
        active.discovery_timeout_ms > 0 &&
            (active.button_mode == MOSAICO_INTERACT_BUTTON_MODE_AUTO ||
             active.button_mode == MOSAICO_INTERACT_BUTTON_MODE_GPIO ||
             active.button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH) &&
            (active.slot == MOSAICO_MODULE_MGR_SLOT_AUTO ||
             (active.slot >= MOSAICO_MODULE_MGR_SLOT_LEFT && active.slot < MOSAICO_MODULE_MGR_SLOT_COUNT)),
        ESP_ERR_INVALID_ARG, TAG, "invalid interact configuration");

    ESP_RETURN_ON_ERROR(mosaico_module_mgr_init(NULL), TAG,
                        "initialize subboard manager failed");

    mosaico_interact_handle_t handle =
        heap_caps_calloc(1, sizeof(*handle),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG,
                        "allocate interact context failed");
    handle->config = active;
    handle->active_input = MOSAICO_INTERACT_BUTTON_MODE_GPIO;
    atomic_init(&handle->touch_l_pressed, false);
    atomic_init(&handle->touch_r_pressed, false);

    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        heap_caps_free(handle);
        return ESP_ERR_NO_MEM;
    }

    const mosaico_module_mgr_claim_config_t claim_config = {
        .expected_type = MOSAICO_BOARD_TYPE_INTERACT,
        .slot = active.slot,
        .timeout_ms = active.discovery_timeout_ms,
    };
    esp_err_t ret = mosaico_module_mgr_claim(&claim_config, &handle->lease);
    if (ret != ESP_OK) {
        goto fail;
    }
    handle->slot = handle->lease.slot;
    handle->subboard_claimed = true;

    ret = get_hw_config((bsp_subboard_slot_t)handle->slot, &handle->hardware);
    if (ret != ESP_OK) {
        goto fail;
    }

    if (handle->hardware.led_count == 0 || handle->hardware.led_count > MOSAICO_INTERACT_LED_COUNT) {
        ESP_LOGE(TAG, "Unsupported LED count: %u", handle->hardware.led_count);
        ret = ESP_ERR_INVALID_SIZE;
        goto fail;
    }

    ret = configure_pir(&handle->hardware);
    if (ret != ESP_OK) {
        goto fail;
    }

    ret = apply_button_mode(handle, active.button_mode);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = create_adc(handle);
    if (ret != ESP_OK) {
        goto fail;
    }
    /* WS2812 before IR: both boards need a strip; IR is allocated on first send.
     * ESP32-S31 has 4 RMT TX channels; two boards x (LED+IR) used to exhaust them. */
    ret = create_led_strip(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initialize LEDs failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = led_strip_clear(handle->strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Clear LEDs failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ESP_LOGI(TAG,
             "Interaction ready: slot=%s eeprom=0x%02X LDR=%d PIR=%d IR=%d KEY_L=%d KEY_R=%d WS2812=%d leds=%s",
             mosaico_module_mgr_slot_to_name(handle->slot),
             handle->hardware.eeprom_addr, handle->hardware.ldr_io,
             handle->hardware.pir_io, handle->hardware.ir_io,
             handle->hardware.key_l_io, handle->hardware.key_r_io,
             handle->hardware.ws2812_io,
             handle->strip ? "rmt" : "off");
    *out_handle = handle;
    return ESP_OK;

fail:
    if (release_resources(handle) != ESP_OK) {
        *out_handle = handle;
        return ret;
    }
    vSemaphoreDelete(handle->lock);
    heap_caps_free(handle);
    return ret;
}

esp_err_t mosaico_interact_read_inputs(mosaico_interact_handle_t handle, mosaico_interact_inputs_t *out_inputs)
{
    ESP_RETURN_ON_FALSE(handle && out_inputs, ESP_ERR_INVALID_ARG, TAG,
                        "invalid interact read request");

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    bool key_l = false;
    bool key_r = false;
    read_keys(handle, &key_l, &key_r);

    int ldr_raw = 0;
    hw_lock();
    esp_err_t ret = adc_oneshot_read(handle->adc, handle->ldr_channel, &ldr_raw);
    hw_unlock();
    if (ret != ESP_OK) {
        handle->ldr_filter_valid = false;
        xSemaphoreGive(handle->lock);
        ESP_RETURN_ON_ERROR(ret, TAG, "read LDR ADC failed");
    }

#if CONFIG_IDF_TARGET_ESP32S31
    // Even channels are N-side inputs: their raw codes decrease as voltage rises.
    const bool inverted = (handle->ldr_channel & 1U) == 0;
    int level = inverted ? LDR_S31_ZERO_CODE - ldr_raw : ldr_raw - LDR_S31_ZERO_CODE;
    const int span = inverted ? LDR_S31_ZERO_CODE : LDR_S31_HIGH_CODE - LDR_S31_ZERO_CODE;
#else
    int level = ldr_raw;
    const int span = LDR_ADC_MAX;
#endif
    if (level < 0) level = 0;
    if (level > span) level = span;
    // Keep fractional precision through filtering; round only the displayed percentage.
    const int target_q8 = (level * 100 * 256 + span / 2) / span;
    if (!handle->ldr_filter_valid) {
        handle->ldr_filtered_q8 = target_q8;
        handle->ldr_filter_valid = true;
        ESP_LOGI(TAG, "LDR GPIO%d channel %d raw=%d level=%d/%d", handle->hardware.ldr_io, handle->ldr_channel, ldr_raw, level, span);
    } else {
        handle->ldr_filtered_q8 = (handle->ldr_filtered_q8 * 3 + target_q8 + 2) / 4;
    }
    const int percent = (handle->ldr_filtered_q8 + 128) / 256;
    const mosaico_interact_inputs_t inputs = {
        .left_pressed = key_l,
        .right_pressed = key_r,
        .motion_detected = gpio_get_level(handle->hardware.pir_io) != 0,
        .light_raw = ldr_raw,
        .light_level = (uint8_t)percent,
    };
    *out_inputs = inputs;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t mosaico_interact_get_info(mosaico_interact_handle_t handle, mosaico_interact_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(handle && out_info, ESP_ERR_INVALID_ARG, TAG, "invalid info request");
    *out_info = (mosaico_interact_info_t) {
        .slot = handle->slot,
        .button_mode = handle->active_input,
        .led_count = handle->hardware.led_count,
    };
    return ESP_OK;
}

esp_err_t mosaico_interact_led_set(mosaico_interact_handle_t handle, uint8_t index, mosaico_interact_rgb_t color)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle is null");
    ESP_RETURN_ON_FALSE(index < handle->hardware.led_count, ESP_ERR_INVALID_ARG,
                        TAG, "LED index %u out of range", index);

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    mosaico_interact_rgb_t colors[MOSAICO_INTERACT_LED_COUNT];
    memcpy(colors, handle->colors, sizeof(colors));
    colors[index] = color;
    esp_err_t ret = push_leds(handle, colors);
    xSemaphoreGive(handle->lock);
    return ret;
}

esp_err_t mosaico_interact_led_write(mosaico_interact_handle_t handle, const mosaico_interact_rgb_t *colors, size_t count)
{
    ESP_RETURN_ON_FALSE(handle && colors, ESP_ERR_INVALID_ARG, TAG,
                        "invalid LED buffer");
    ESP_RETURN_ON_FALSE(count == handle->hardware.led_count, ESP_ERR_INVALID_ARG,
                        TAG, "LED count %zu must equal %u", count, handle->hardware.led_count);

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = push_leds(handle, colors);
    xSemaphoreGive(handle->lock);
    return ret;
}

esp_err_t mosaico_interact_led_fill(mosaico_interact_handle_t handle, mosaico_interact_rgb_t color)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle is null");
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    mosaico_interact_rgb_t colors[MOSAICO_INTERACT_LED_COUNT];
    for (uint8_t i = 0; i < handle->hardware.led_count; ++i) {
        colors[i] = color;
    }
    esp_err_t ret = push_leds(handle, colors);
    xSemaphoreGive(handle->lock);
    return ret;
}

esp_err_t mosaico_interact_led_clear(mosaico_interact_handle_t handle)
{
    const mosaico_interact_rgb_t off = {0, 0, 0};
    return mosaico_interact_led_fill(handle, off);
}

esp_err_t mosaico_interact_ir_send_nec(mosaico_interact_handle_t handle, uint8_t address, uint8_t command)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle is null");

    const rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };

    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = create_ir(handle);
    if (ret != ESP_OK) {
        xSemaphoreGive(handle->lock);
        ESP_LOGE(TAG, "IR RMT unavailable on slot=%s: %s",
                 mosaico_module_mgr_slot_to_name(handle->slot),
                 esp_err_to_name(ret));
        return ret;
    }
    // RMT retains the payload until completion, including timeout recovery.
    const size_t count = build_nec_symbols(address, command, handle->ir_symbols);
    ret = rmt_transmit(handle->ir_chan, handle->ir_encoder, handle->ir_symbols,
                      count * sizeof(handle->ir_symbols[0]), &tx_cfg);
    if (ret == ESP_OK) {
        ret = rmt_tx_wait_all_done(handle->ir_chan, 100);
    }
    if (ret != ESP_OK) {
        release_ir(handle);
    }
    xSemaphoreGive(handle->lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IR NEC 0x%02X/0x%02X failed: %s", address, command,
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "IR NEC sent addr=0x%02X cmd=0x%02X", address, command);
    }
    return ret;
}

esp_err_t mosaico_interact_close(mosaico_interact_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle is null");
    if (xSemaphoreTake(handle->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    const mosaico_module_mgr_slot_t slot = handle->slot;
    esp_err_t ret = release_resources(handle);
    if (ret != ESP_OK) {
        xSemaphoreGive(handle->lock);
        return ret;
    }
    xSemaphoreGive(handle->lock);
    vSemaphoreDelete(handle->lock);
    heap_caps_free(handle);
    ESP_LOGI(TAG, "Interaction closed; slot=%s discovery resumed",
             mosaico_module_mgr_slot_to_name(slot));
    return ESP_OK;
}
