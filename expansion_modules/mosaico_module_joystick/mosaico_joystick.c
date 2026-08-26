/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_joystick.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

static const char *TAG = "mosaico_joystick";

#if SOC_ADC_ATTEN_NUM <= 1
#define JOYSTICK_ADC_ATTEN ADC_ATTEN_DB_0
#else
#define JOYSTICK_ADC_ATTEN ADC_ATTEN_DB_12
#endif

typedef struct {
    adc_unit_t unit;
    adc_oneshot_unit_handle_t handle;
    size_t refs;
} shared_adc_unit_t;

typedef struct {
    int64_t sum_x;
    int64_t sum_y;
    int samples;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    uint64_t since_ms;
} stable_window_t;

struct mosaico_joystick_t {
    mosaico_joystick_config_t config;
    mosaico_module_mgr_slot_t slot;
    uint32_t claim_generation;
    bsp_subboard_joystick_config_t hardware;
    adc_unit_t unit_x;
    adc_unit_t unit_y;
    adc_channel_t channel_x;
    adc_channel_t channel_y;
    adc_oneshot_unit_handle_t adc_x;
    adc_oneshot_unit_handle_t adc_y;
    bool unit_x_acquired;
    bool unit_y_acquired;
    bool subboard_claimed;
    SemaphoreHandle_t lock;
    mosaico_joystick_data_t data;
    uint64_t phase_since_ms;
    uint64_t center_deadline_since_ms;
    stable_window_t center;
    stable_window_t idle;
};

static shared_adc_unit_t s_adc_units[] = {
    {.unit = ADC_UNIT_1},
    {.unit = ADC_UNIT_2},
};
static StaticSemaphore_t s_adc_lock_storage;
static SemaphoreHandle_t s_adc_lock;
static portMUX_TYPE s_adc_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static SemaphoreHandle_t adc_registry_lock(void)
{
    portENTER_CRITICAL(&s_adc_lock_init_mux);
    if (!s_adc_lock) {
        s_adc_lock = xSemaphoreCreateMutexStatic(&s_adc_lock_storage);
    }
    SemaphoreHandle_t lock = s_adc_lock;
    portEXIT_CRITICAL(&s_adc_lock_init_mux);
    return lock;
}

static shared_adc_unit_t *find_adc_unit(adc_unit_t unit)
{
    for (size_t i = 0; i < sizeof(s_adc_units) / sizeof(s_adc_units[0]); ++i) {
        if (s_adc_units[i].unit == unit) {
            return &s_adc_units[i];
        }
    }
    return NULL;
}

static esp_err_t acquire_adc_unit(adc_unit_t unit,
                                  adc_oneshot_unit_handle_t *out_handle)
{
    shared_adc_unit_t *shared = find_adc_unit(unit);
    ESP_RETURN_ON_FALSE(shared && out_handle, ESP_ERR_INVALID_ARG, TAG,
                        "unsupported ADC unit");

    SemaphoreHandle_t lock = adc_registry_lock();
    ESP_RETURN_ON_FALSE(lock, ESP_ERR_NO_MEM, TAG,
                        "create shared ADC lock failed");
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (!shared->handle) {
        const adc_oneshot_unit_init_cfg_t config = {
            .unit_id = unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ret = adc_oneshot_new_unit(&config, &shared->handle);
    }
    if (ret == ESP_OK) {
        shared->refs++;
        *out_handle = shared->handle;
    }
    xSemaphoreGive(lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create ADC unit %d failed: %s", unit,
                 esp_err_to_name(ret));
    }
    return ret;
}

static void release_adc_unit(adc_unit_t unit)
{
    shared_adc_unit_t *shared = find_adc_unit(unit);
    SemaphoreHandle_t lock = adc_registry_lock();
    if (!shared || !lock) {
        return;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    if (shared->refs > 0) {
        shared->refs--;
    }
    if (shared->refs == 0 && shared->handle) {
        esp_err_t ret = adc_oneshot_del_unit(shared->handle);
        if (ret == ESP_OK) {
            shared->handle = NULL;
        } else {
            ESP_LOGE(TAG, "Delete ADC unit %d failed: %s", unit,
                     esp_err_to_name(ret));
        }
    }
    xSemaphoreGive(lock);
}

static esp_err_t validate_config(const mosaico_joystick_config_t *config)
{
    ESP_RETURN_ON_FALSE(
        (config->slot == MOSAICO_MODULE_MGR_SLOT_AUTO ||
         config->slot < MOSAICO_MODULE_MGR_SLOT_COUNT) &&
            config->discovery_timeout_ms > 0 && config->oversample > 0 &&
            config->deadzone >= 0.0f && config->deadzone < 1.0f &&
            config->circle_ms > 0 && config->center_ms > 0 &&
            config->center_max_ms >= config->center_ms &&
            config->stable_span >= 0 && config->center_stable_span >= 0 &&
            config->idle_recenter_ms > 0 && config->min_range_warning >= 0 &&
            config->min_span_finish > 0,
        ESP_ERR_INVALID_ARG, TAG, "invalid joystick configuration");
    return ESP_OK;
}

static esp_err_t wait_for_available_joystick(
    mosaico_module_mgr_slot_t preferred_slot, uint32_t timeout_ms,
    mosaico_module_mgr_info_t *out_info)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    do {
        for (size_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
            const mosaico_module_mgr_slot_t slot =
                preferred_slot == MOSAICO_MODULE_MGR_SLOT_AUTO
                    ? (mosaico_module_mgr_slot_t)i
                    : preferred_slot;
            mosaico_module_mgr_info_t info = {0};
            esp_err_t ret = mosaico_module_mgr_get_info(slot, &info);
            if (ret != ESP_OK) {
                return ret;
            }
            if (info.state == MOSAICO_MODULE_MGR_STATE_READY &&
                info.eeprom.board_type == MOSAICO_BOARD_TYPE_HANDLE) {
                *out_info = info;
                return ESP_OK;
            }
            if (preferred_slot != MOSAICO_MODULE_MGR_SLOT_AUTO) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (xTaskGetTickCount() - start < timeout);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t check_present_locked(mosaico_joystick_handle_t handle)
{
    mosaico_module_mgr_info_t info = {0};
    esp_err_t ret = mosaico_module_mgr_get_info(handle->slot, &info);
    if (ret != ESP_OK) {
        return ret;
    }
    if (info.state != MOSAICO_MODULE_MGR_STATE_CLAIMED ||
        info.generation != handle->claim_generation ||
        info.eeprom.board_type != MOSAICO_BOARD_TYPE_HANDLE) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void stable_window_start(stable_window_t *window, int x, int y,
                                uint64_t time_ms)
{
    *window = (stable_window_t) {
        .sum_x = x,
        .sum_y = y,
        .samples = 1,
        .min_x = x,
        .max_x = x,
        .min_y = y,
        .max_y = y,
        .since_ms = time_ms,
    };
}

static bool stable_window_add(stable_window_t *window, int x, int y,
                              int max_span, uint64_t time_ms)
{
    if (window->samples == 0) {
        stable_window_start(window, x, y, time_ms);
        return false;
    }

    if (x < window->min_x) {
        window->min_x = x;
    }
    if (x > window->max_x) {
        window->max_x = x;
    }
    if (y < window->min_y) {
        window->min_y = y;
    }
    if (y > window->max_y) {
        window->max_y = y;
    }
    if (window->max_x - window->min_x > max_span ||
        window->max_y - window->min_y > max_span) {
        stable_window_start(window, x, y, time_ms);
        return false;
    }

    window->sum_x += x;
    window->sum_y += y;
    window->samples++;
    return true;
}

static void begin_calibration(mosaico_joystick_handle_t handle)
{
    handle->data.state = MOSAICO_JOYSTICK_CAL_CIRCLE;
    handle->data.raw_x = 0;
    handle->data.raw_y = 0;
    handle->data.x = 0.0f;
    handle->data.y = 0.0f;
    memset(handle->data.buttons, 0, sizeof(handle->data.buttons));
    handle->data.calibration = (mosaico_joystick_calibration_t) {
        .min_x = INT_MAX,
        .max_x = INT_MIN,
        .min_y = INT_MAX,
        .max_y = INT_MIN,
    };
    memset(&handle->center, 0, sizeof(handle->center));
    memset(&handle->idle, 0, sizeof(handle->idle));
    handle->phase_since_ms = now_ms();
    ESP_LOGI(TAG, "%s joystick calibration started: rotate full circle",
             mosaico_module_mgr_slot_to_name(handle->slot));
}

static void expand_range(mosaico_joystick_handle_t handle)
{
    mosaico_joystick_calibration_t *cal = &handle->data.calibration;
    const int x = handle->data.raw_x;
    const int y = handle->data.raw_y;
    if (x < cal->min_x) {
        cal->min_x = x;
    }
    if (x > cal->max_x) {
        cal->max_x = x;
    }
    if (y < cal->min_y) {
        cal->min_y = y;
    }
    if (y > cal->max_y) {
        cal->max_y = y;
    }
    if (cal->center_x < cal->min_x) {
        cal->center_x = cal->min_x;
    }
    if (cal->center_x > cal->max_x) {
        cal->center_x = cal->max_x;
    }
    if (cal->center_y < cal->min_y) {
        cal->center_y = cal->min_y;
    }
    if (cal->center_y > cal->max_y) {
        cal->center_y = cal->max_y;
    }
}

static float normalize_axis(int raw, int min_value, int center, int max_value,
                            float deadzone)
{
    float normalized = 0.0f;
    const int span = raw >= center ? max_value - center : center - min_value;
    if (span > 0) {
        normalized = (float)(raw - center) / (float)span;
    }
    if (normalized > 1.0f) {
        normalized = 1.0f;
    } else if (normalized < -1.0f) {
        normalized = -1.0f;
    }
    return (normalized > -deadzone && normalized < deadzone) ? 0.0f
                                                              : normalized;
}

static void update_normalized(mosaico_joystick_handle_t handle)
{
    const mosaico_joystick_calibration_t *cal = &handle->data.calibration;
    float x = normalize_axis(handle->data.raw_x, cal->min_x, cal->center_x,
                             cal->max_x, handle->config.deadzone);
    float y = normalize_axis(handle->data.raw_y, cal->min_y, cal->center_y,
                             cal->max_y, handle->config.deadzone);
    if (handle->hardware.invert_x) {
        x = -x;
    }
    if (handle->hardware.invert_y) {
        y = -y;
    }
    const float magnitude_squared = x * x + y * y;
    if (magnitude_squared > 1.0f) {
        const float scale = 1.0f / sqrtf(magnitude_squared);
        x *= scale;
        y *= scale;
    }
    handle->data.x = x;
    handle->data.y = y;
}

static bool any_button_pressed(mosaico_joystick_handle_t handle)
{
    for (size_t i = 0; i < MOSAICO_JOYSTICK_BUTTON_COUNT; ++i) {
        if (handle->data.buttons[i]) {
            return true;
        }
    }
    return false;
}

static void finish_circle(mosaico_joystick_handle_t handle, uint64_t time_ms)
{
    mosaico_joystick_calibration_t *cal = &handle->data.calibration;
    cal->center_x = (cal->min_x + cal->max_x) / 2;
    cal->center_y = (cal->min_y + cal->max_y) / 2;
    const int span_x = cal->max_x - cal->min_x;
    const int span_y = cal->max_y - cal->min_y;
    ESP_LOGI(TAG, "%s circle done: X[%d..%d] Y[%d..%d]",
             mosaico_module_mgr_slot_to_name(handle->slot), cal->min_x,
             cal->max_x, cal->min_y, cal->max_y);
    if (span_x < handle->config.min_range_warning ||
        span_y < handle->config.min_range_warning) {
        ESP_LOGW(TAG, "%s joystick calibration range is small: X=%d Y=%d",
                 mosaico_module_mgr_slot_to_name(handle->slot), span_x, span_y);
    }

    handle->data.state = MOSAICO_JOYSTICK_CAL_CENTER;
    handle->phase_since_ms = time_ms;
    handle->center_deadline_since_ms = time_ms;
    stable_window_start(&handle->center, handle->data.raw_x,
                        handle->data.raw_y, time_ms);
    ESP_LOGI(TAG, "%s joystick: release and hold at center",
             mosaico_module_mgr_slot_to_name(handle->slot));
}

static void finish_center(mosaico_joystick_handle_t handle, bool geometric)
{
    mosaico_joystick_calibration_t *cal = &handle->data.calibration;
    if (!geometric && handle->center.samples > 0) {
        const int center_x =
            (int)(handle->center.sum_x / handle->center.samples);
        const int center_y =
            (int)(handle->center.sum_y / handle->center.samples);
        if (center_x >= cal->min_x && center_x <= cal->max_x) {
            cal->center_x = center_x;
        }
        if (center_y >= cal->min_y && center_y <= cal->max_y) {
            cal->center_y = center_y;
        }
    }
    handle->data.state = MOSAICO_JOYSTICK_READY;
    memset(&handle->idle, 0, sizeof(handle->idle));
    update_normalized(handle);
    ESP_LOGI(TAG, "%s joystick ready: center=(%d,%d) X[%d..%d] Y[%d..%d]",
             mosaico_module_mgr_slot_to_name(handle->slot), cal->center_x,
             cal->center_y, cal->min_x, cal->max_x, cal->min_y, cal->max_y);
}

static void idle_recenter(mosaico_joystick_handle_t handle, uint64_t time_ms)
{
    stable_window_t *idle = &handle->idle;
    stable_window_add(idle, handle->data.raw_x, handle->data.raw_y,
                      handle->config.stable_span, time_ms);
    if (time_ms - idle->since_ms < handle->config.idle_recenter_ms) {
        return;
    }

    mosaico_joystick_calibration_t *cal = &handle->data.calibration;
    const int center_x = (int)(idle->sum_x / idle->samples);
    const int center_y = (int)(idle->sum_y / idle->samples);
    const int geometric_x = (cal->min_x + cal->max_x) / 2;
    const int geometric_y = (cal->min_y + cal->max_y) / 2;
    const int tolerance_x = (cal->max_x - cal->min_x) / 3;
    const int tolerance_y = (cal->max_y - cal->min_y) / 3;
    if (center_x >= cal->min_x && center_x <= cal->max_x &&
        center_y >= cal->min_y && center_y <= cal->max_y &&
        abs(center_x - geometric_x) <= tolerance_x &&
        abs(center_y - geometric_y) <= tolerance_y) {
        cal->center_x = center_x;
        cal->center_y = center_y;
    }
    memset(idle, 0, sizeof(*idle));
}

static esp_err_t configure_hardware(mosaico_joystick_handle_t handle)
{
    ESP_RETURN_ON_ERROR(
        adc_oneshot_io_to_channel(handle->hardware.x_io, &handle->unit_x,
                                  &handle->channel_x),
        TAG, "map joystick X GPIO to ADC failed");
    ESP_RETURN_ON_ERROR(
        adc_oneshot_io_to_channel(handle->hardware.y_io, &handle->unit_y,
                                  &handle->channel_y),
        TAG, "map joystick Y GPIO to ADC failed");

    ESP_RETURN_ON_ERROR(acquire_adc_unit(handle->unit_x, &handle->adc_x), TAG,
                        "acquire joystick X ADC failed");
    handle->unit_x_acquired = true;
    if (handle->unit_y == handle->unit_x) {
        handle->adc_y = handle->adc_x;
    } else {
        esp_err_t ret = acquire_adc_unit(handle->unit_y, &handle->adc_y);
        if (ret != ESP_OK) {
            return ret;
        }
        handle->unit_y_acquired = true;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = JOYSTICK_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    SemaphoreHandle_t adc_lock = adc_registry_lock();
    xSemaphoreTake(adc_lock, portMAX_DELAY);
    esp_err_t ret = adc_oneshot_config_channel(handle->adc_x, handle->channel_x,
                                               &channel_config);
    if (ret == ESP_OK) {
        ret = adc_oneshot_config_channel(handle->adc_y, handle->channel_y,
                                         &channel_config);
    }
    xSemaphoreGive(adc_lock);
    ESP_RETURN_ON_ERROR(ret, TAG, "configure joystick ADC channel failed");

    for (size_t i = 0; i < MOSAICO_JOYSTICK_BUTTON_COUNT; ++i) {
        const bsp_subboard_joystick_button_config_t *button =
            &handle->hardware.buttons[i];
        const gpio_config_t button_config = {
            .pin_bit_mask = BIT64(button->io),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en =
                button->active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
            .pull_down_en = button->active_high ? GPIO_PULLDOWN_ENABLE
                                                : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG,
                            "configure joystick button GPIO%d failed",
                            button->io);
    }
    return ESP_OK;
}

static void release_resources(mosaico_joystick_handle_t handle)
{
    if (!handle) {
        return;
    }
    for (size_t i = 0; i < MOSAICO_JOYSTICK_BUTTON_COUNT; ++i) {
        gpio_reset_pin(handle->hardware.buttons[i].io);
    }
    gpio_reset_pin(handle->hardware.x_io);
    gpio_reset_pin(handle->hardware.y_io);
    if (handle->unit_y_acquired) {
        release_adc_unit(handle->unit_y);
        handle->unit_y_acquired = false;
    }
    if (handle->unit_x_acquired) {
        release_adc_unit(handle->unit_x);
        handle->unit_x_acquired = false;
    }
    if (handle->subboard_claimed) {
        esp_err_t ret = mosaico_module_mgr_release(handle->slot);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Release joystick slot %s failed: %s",
                     mosaico_module_mgr_slot_to_name(handle->slot),
                     esp_err_to_name(ret));
        }
        handle->subboard_claimed = false;
    }
}

esp_err_t mosaico_joystick_new(const mosaico_joystick_config_t *config,
                               mosaico_joystick_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG,
                        "joystick output handle is null");
    *out_handle = NULL;
    mosaico_joystick_config_t active =
        config ? *config
               : (mosaico_joystick_config_t)MOSAICO_JOYSTICK_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(validate_config(&active), TAG,
                        "validate joystick configuration failed");
    ESP_RETURN_ON_ERROR(mosaico_module_mgr_init(NULL), TAG,
                        "initialize module manager failed");

    mosaico_module_mgr_info_t info = {0};
    ESP_RETURN_ON_ERROR(wait_for_available_joystick(
                            active.slot, active.discovery_timeout_ms, &info),
                        TAG, "discover joystick subboard failed");

    mosaico_joystick_handle_t handle =
        heap_caps_calloc(1, sizeof(*handle), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG,
                        "allocate joystick context failed");
    handle->config = active;
    handle->slot = info.slot;
    handle->data.slot = info.slot;
    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        heap_caps_free(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret =
        mosaico_module_mgr_claim(handle->slot, MOSAICO_BOARD_TYPE_HANDLE);
    if (ret != ESP_OK) {
        goto fail;
    }
    handle->subboard_claimed = true;
    ret = mosaico_module_mgr_get_info(handle->slot, &info);
    if (ret != ESP_OK) {
        goto fail;
    }
    handle->claim_generation = info.generation;

    ret = bsp_subboard_joystick_get_config((bsp_subboard_slot_t)handle->slot,
                                           &handle->hardware);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = configure_hardware(handle);
    if (ret != ESP_OK) {
        goto fail;
    }
    begin_calibration(handle);
    ESP_LOGI(TAG, "Joystick opened: slot=%s eeprom=0x%02X X=%d Y=%d",
             mosaico_module_mgr_slot_to_name(handle->slot),
             handle->hardware.eeprom_addr, handle->hardware.x_io,
             handle->hardware.y_io);
    *out_handle = handle;
    return ESP_OK;

fail:
    release_resources(handle);
    vSemaphoreDelete(handle->lock);
    heap_caps_free(handle);
    return ret;
}

static esp_err_t update_locked(mosaico_joystick_handle_t handle)
{
    esp_err_t ret = check_present_locked(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    int64_t sum_x = 0;
    int64_t sum_y = 0;
    SemaphoreHandle_t adc_lock = adc_registry_lock();
    xSemaphoreTake(adc_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < handle->config.oversample; ++i) {
        int x = 0;
        int y = 0;
        ret = adc_oneshot_read(handle->adc_x, handle->channel_x, &x);
        if (ret == ESP_OK) {
            ret = adc_oneshot_read(handle->adc_y, handle->channel_y, &y);
        }
        if (ret != ESP_OK) {
            break;
        }
        sum_x += x;
        sum_y += y;
    }
    xSemaphoreGive(adc_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read %s joystick ADC failed: %s",
                 mosaico_module_mgr_slot_to_name(handle->slot),
                 esp_err_to_name(ret));
        return ret;
    }
    handle->data.raw_x = (int)(sum_x / handle->config.oversample);
    handle->data.raw_y = (int)(sum_y / handle->config.oversample);
    for (size_t i = 0; i < MOSAICO_JOYSTICK_BUTTON_COUNT; ++i) {
        const bsp_subboard_joystick_button_config_t *button =
            &handle->hardware.buttons[i];
        const int level = gpio_get_level(button->io);
        handle->data.buttons[i] =
            button->active_high ? level != 0 : level == 0;
    }

    const uint64_t time_ms = now_ms();
    if (handle->data.state == MOSAICO_JOYSTICK_CAL_CIRCLE) {
        expand_range(handle);
        mosaico_joystick_calibration_t *cal = &handle->data.calibration;
        cal->center_x = (cal->min_x + cal->max_x) / 2;
        cal->center_y = (cal->min_y + cal->max_y) / 2;
        update_normalized(handle);
        const bool enough_range =
            cal->max_x - cal->min_x >= handle->config.min_span_finish &&
            cal->max_y - cal->min_y >= handle->config.min_span_finish;
        if (time_ms - handle->phase_since_ms >= handle->config.circle_ms ||
            (enough_range && any_button_pressed(handle))) {
            finish_circle(handle, time_ms);
        }
    } else if (handle->data.state == MOSAICO_JOYSTICK_CAL_CENTER) {
        stable_window_add(&handle->center, handle->data.raw_x,
                          handle->data.raw_y,
                          handle->config.center_stable_span, time_ms);
        mosaico_joystick_calibration_t *cal = &handle->data.calibration;
        cal->center_x = (int)(handle->center.sum_x / handle->center.samples);
        cal->center_y = (int)(handle->center.sum_y / handle->center.samples);
        update_normalized(handle);
        if (time_ms - handle->center.since_ms >= handle->config.center_ms) {
            finish_center(handle, false);
        } else if (time_ms - handle->center_deadline_since_ms >=
                   handle->config.center_max_ms) {
            ESP_LOGW(TAG, "%s joystick center is noisy; using geometric center",
                     mosaico_module_mgr_slot_to_name(handle->slot));
            finish_center(handle, true);
        }
    } else {
        expand_range(handle);
        idle_recenter(handle, time_ms);
        update_normalized(handle);
    }
    return ESP_OK;
}

esp_err_t mosaico_joystick_read(mosaico_joystick_handle_t handle,
                                mosaico_joystick_data_t *out_data)
{
    ESP_RETURN_ON_FALSE(handle && out_data, ESP_ERR_INVALID_ARG, TAG,
                        "invalid joystick read request");
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    esp_err_t ret = update_locked(handle);
    if (ret == ESP_OK) {
        *out_data = handle->data;
    }
    xSemaphoreGive(handle->lock);
    return ret;
}

esp_err_t mosaico_joystick_recalibrate(mosaico_joystick_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG,
                        "joystick handle is null");
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    esp_err_t ret = check_present_locked(handle);
    if (ret == ESP_OK) {
        begin_calibration(handle);
    }
    xSemaphoreGive(handle->lock);
    return ret;
}

esp_err_t mosaico_joystick_del(mosaico_joystick_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG,
                        "joystick handle is null");
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    const mosaico_module_mgr_slot_t slot = handle->slot;
    release_resources(handle);
    xSemaphoreGive(handle->lock);
    vSemaphoreDelete(handle->lock);
    heap_caps_free(handle);
    ESP_LOGI(TAG, "Joystick closed: slot=%s",
             mosaico_module_mgr_slot_to_name(slot));
    return ESP_OK;
}
