/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include "mosaico_module_tof.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "bsp/power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vl53l0x_def.h"
#include "vl53l4cd_api.h"

static const char *TAG = "mosaico_tof";

typedef enum {
    SENSOR_UNKNOWN,
    SENSOR_VL53L0X,
    SENSOR_VL53L4CD,
} sensor_model_t;

struct mosaico_tof {
    sensor_model_t model;
    i2c_master_bus_handle_t bus;
    gpio_num_t xshut;
    vl53l0x_handle_t l0x;
    mosaico_l4cd_platform_t l4cd;
    bool ranging;
};

static esp_err_t reset_sensor(gpio_num_t xshut)
{
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG, "enable module rail failed");
    vTaskDelay(pdMS_TO_TICKS(200) + 1);
    ESP_RETURN_ON_ERROR(gpio_reset_pin(xshut), TAG, "reset XSHUT GPIO failed");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(xshut), TAG, "release XSHUT hold failed");
    ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis(xshut), TAG, "disable XSHUT sleep selection failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(xshut, 0), TAG, "preset XSHUT failed");
    const gpio_config_t gpio = {
        .pin_bit_mask = 1ULL << xshut,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio), TAG, "configure XSHUT failed");
    /* Round up so the reset pulse survives low FreeRTOS tick rates. */
    vTaskDelay(pdMS_TO_TICKS(2) + 1);
    ESP_RETURN_ON_ERROR(gpio_set_level(xshut, 1), TAG, "release XSHUT failed");
    vTaskDelay(pdMS_TO_TICKS(50) + 1);
    return ESP_OK;
}

static esp_err_t l4cd_status(mosaico_tof_handle_t sensor, const char *step, uint8_t status)
{
    esp_err_t ret = sensor->l4cd.error;
    if (ret == ESP_OK && status != VL53L4CD_ERROR_NONE) {
        ret = status == VL53L4CD_ERROR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VL53L4CD %s failed: status=%u error=%s", step, status, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t identify_sensor(mosaico_tof_handle_t sensor)
{
    /* Acknowledge alone cannot identify the model: both parts use 0x29. */
    const uint8_t l0x_id_register = 0xC0;
    uint8_t model_id = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(sensor->l4cd.i2c,
                        &l0x_id_register, 1, &model_id, 1, 100), TAG, "read sensor model failed");
    if (model_id == 0xEE) {
        sensor->model = SENSOR_VL53L0X;
        ESP_LOGD(TAG, "Detected VL53L0X: 8-bit register 0xC0=0xEE");
        return ESP_OK;
    }

    /* VL53L4CD needs a two-byte register index, unlike VL53L0X. */
    uint16_t id = 0;
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "read ID", VL53L4CD_GetSensorId(&sensor->l4cd, &id)),
                        TAG, "read 16-bit sensor identity failed");
    if (id == 0xEBAA) {
        sensor->model = SENSOR_VL53L4CD;
        ESP_LOGD(TAG, "Detected VL53L4CD: 16-bit register 0x010F=0x%04X", id);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Unsupported sensor at 0x29: 8-bit ID=0x%02X, 16-bit ID=0x%04X", model_id, id);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t open_sensor(bsp_subboard_slot_t slot, mosaico_tof_handle_t *out_sensor, bool allow_l4cd)
{
    ESP_RETURN_ON_FALSE(out_sensor, ESP_ERR_INVALID_ARG, TAG, "null output handle");
    *out_sensor = NULL;
    ESP_RETURN_ON_FALSE(slot >= BSP_SUBBOARD_SLOT_LEFT && slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid slot");
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "subboard bus not initialized");
    mosaico_tof_handle_t sensor = calloc(1, sizeof(*sensor));
    ESP_RETURN_ON_FALSE(sensor, ESP_ERR_NO_MEM, TAG, "allocate sensor failed");
    sensor->bus = bus;
    sensor->xshut = bsp_subboard_map_gpio(slot, GPIO_NUM_48);
    esp_err_t ret;
    const i2c_device_config_t config = {
        .device_address = VL53L0X_I2C_DEFAULT_ADDR_7BIT,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = VL53L0X_I2C_DEFAULT_FREQ_HZ,
    };
    ESP_GOTO_ON_ERROR(reset_sensor(sensor->xshut), fail, TAG, "sensor reset failed");
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(bus, &config, &sensor->l4cd.i2c), fail, TAG, "add sensor failed");
    ESP_GOTO_ON_ERROR(identify_sensor(sensor), fail, TAG, "sensor identification failed");
    if (sensor->model == SENSOR_VL53L4CD) {
        if (!allow_l4cd) {
            ESP_LOGE(TAG, "VL53L4CD requires mosaico_tof_open(); the legacy API only supports VL53L0X");
            ret = ESP_ERR_NOT_SUPPORTED;
            goto fail;
        }
        ESP_GOTO_ON_ERROR(l4cd_status(sensor, "initialize", VL53L4CD_SensorInit(&sensor->l4cd)), fail, TAG, "initialize VL53L4CD failed");
        ESP_GOTO_ON_ERROR(l4cd_status(sensor, "set timing", VL53L4CD_SetRangeTiming(&sensor->l4cd, 33, 0)), fail, TAG, "set VL53L4CD timing failed");
        ESP_GOTO_ON_ERROR(l4cd_status(sensor, "start ranging", VL53L4CD_StartRanging(&sensor->l4cd)), fail, TAG, "start VL53L4CD failed");
        sensor->ranging = true;
    } else {
        /* Release the identification handle before creating the legacy driver. */
        ESP_GOTO_ON_ERROR(i2c_master_bus_rm_device(sensor->l4cd.i2c), fail, TAG, "remove identification handle failed");
        sensor->l4cd.i2c = NULL;
        vl53l0x_ref_calibration_t calibration = {0};
        ESP_GOTO_ON_ERROR(vl53l0x_create(&sensor->l0x, bus), fail, TAG, "create VL53L0X failed");
        ESP_GOTO_ON_ERROR(vl53l0x_init(sensor->l0x), fail, TAG, "initialize VL53L0X failed");
        ESP_GOTO_ON_ERROR(vl53l0x_set_timing_budget(sensor->l0x, VL53L0X_DEFAULT_TIMING_BUDGET_US), fail, TAG, "set timing budget failed");
        ESP_GOTO_ON_ERROR(vl53l0x_perform_ref_calibration(sensor->l0x, &calibration), fail, TAG, "reference calibration failed");
        ESP_GOTO_ON_ERROR(vl53l0x_set_profile(sensor->l0x, VL53L0X_PROFILE_DEFAULT), fail, TAG, "set profile failed");
        ESP_GOTO_ON_ERROR(vl53l0x_set_mode(sensor->l0x, VL53L0X_MODE_SINGLE), fail, TAG, "set single mode failed");
    }
    *out_sensor = sensor;
    ESP_LOGI(TAG, "%s ready: slot=%s XSHUT=%d", mosaico_tof_model_name(sensor),
             slot == BSP_SUBBOARD_SLOT_LEFT ? "left" : "right", sensor->xshut);
    return ESP_OK;

fail:
    /* Keep the caller's slot lease until every host resource has been released. */
    while (mosaico_tof_close(sensor) != ESP_OK) {
        ESP_LOGE(TAG, "Sensor initialization cleanup failed; retrying");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ret;
}

esp_err_t mosaico_tof_open(bsp_subboard_slot_t slot, mosaico_tof_handle_t *out_sensor)
{
    return open_sensor(slot, out_sensor, true);
}

esp_err_t mosaico_tof_new(bsp_subboard_slot_t slot, vl53l0x_handle_t *out_sensor)
{
    ESP_RETURN_ON_FALSE(out_sensor, ESP_ERR_INVALID_ARG, TAG, "null output handle");
    *out_sensor = NULL;
    mosaico_tof_handle_t sensor = NULL;
    esp_err_t ret = open_sensor(slot, &sensor, false);
    if (ret == ESP_OK) {
        *out_sensor = sensor->l0x;
        /* Transfer the legacy handle to the caller without closing it. */
        free(sensor);
    }
    return ret;
}

const char *mosaico_tof_model_name(mosaico_tof_handle_t sensor)
{
    if (!sensor) {
        return "UNKNOWN";
    }
    return sensor->model == SENSOR_VL53L0X ? "VL53L0X" :
           sensor->model == SENSOR_VL53L4CD ? "VL53L4CD" : "UNKNOWN";
}

esp_err_t mosaico_tof_set_address(mosaico_tof_handle_t sensor, uint8_t address)
{
    ESP_RETURN_ON_FALSE(sensor && address >= 0x08 && address <= 0x77 && address != 0x29,
                        ESP_ERR_INVALID_ARG, TAG, "invalid sensor address");
    if (sensor->model == SENSOR_VL53L0X) {
        ESP_RETURN_ON_FALSE(!sensor->ranging, ESP_ERR_INVALID_STATE, TAG, "configure address before continuous ranging");
        return vl53l0x_set_address(sensor->l0x, sensor->bus, address);
    }
    ESP_RETURN_ON_FALSE(sensor->model == SENSOR_VL53L4CD, ESP_ERR_INVALID_STATE, TAG, "unknown sensor model");
    /* ST's API takes the 8-bit address; ESP-IDF takes the 7-bit address. */
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "set address", VL53L4CD_SetI2CAddress(&sensor->l4cd, address << 1)),
                        TAG, "set VL53L4CD address failed");
    ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(sensor->l4cd.i2c), TAG, "remove old I2C device failed");
    sensor->l4cd.i2c = NULL;
    const i2c_device_config_t config = {
        .device_address = address,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = VL53L0X_I2C_DEFAULT_FREQ_HZ,
    };
    return i2c_master_bus_add_device(sensor->bus, &config, &sensor->l4cd.i2c);
}

esp_err_t mosaico_tof_set_timing_budget(mosaico_tof_handle_t sensor, uint32_t budget_us)
{
    ESP_RETURN_ON_FALSE(sensor, ESP_ERR_INVALID_ARG, TAG, "null sensor handle");
    if (sensor->model == SENSOR_VL53L0X) {
        ESP_RETURN_ON_FALSE(!sensor->ranging, ESP_ERR_INVALID_STATE, TAG, "configure timing before continuous ranging");
        return vl53l0x_set_timing_budget(sensor->l0x, budget_us);
    }
    ESP_RETURN_ON_FALSE(sensor->model == SENSOR_VL53L4CD && budget_us >= 10000 &&
                        budget_us <= 200000 && budget_us % 1000 == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid VL53L4CD timing budget");
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "stop for timing", VL53L4CD_StopRanging(&sensor->l4cd)), TAG, "stop VL53L4CD failed");
    sensor->ranging = false;
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "set timing", VL53L4CD_SetRangeTiming(&sensor->l4cd, budget_us / 1000, 0)), TAG, "set VL53L4CD timing failed");
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "restart ranging", VL53L4CD_StartRanging(&sensor->l4cd)), TAG, "restart VL53L4CD failed");
    sensor->ranging = true;
    return ESP_OK;
}

esp_err_t mosaico_tof_get_offset_calibration(mosaico_tof_handle_t sensor, int32_t *offset_um)
{
    ESP_RETURN_ON_FALSE(sensor && offset_um, ESP_ERR_INVALID_ARG, TAG, "invalid offset arguments");
    if (sensor->model == SENSOR_VL53L0X) {
        return vl53l0x_get_offset_calibration(sensor->l0x, offset_um);
    }
    ESP_RETURN_ON_FALSE(sensor->model == SENSOR_VL53L4CD, ESP_ERR_INVALID_STATE, TAG, "unknown sensor model");
    uint16_t value = 0;
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "read offset", VL53L4CD_RdWord(&sensor->l4cd, VL53L4CD_RANGE_OFFSET_MM, &value)), TAG, "read offset failed");
    /* Keep the signed 13-bit quarter-millimeter value, including factory fractions. */
    int32_t quarters = value & 0x1FFF;
    if (quarters & 0x1000) quarters -= 0x2000;
    *offset_um = quarters * 250;
    return ESP_OK;
}

esp_err_t mosaico_tof_set_offset_calibration(mosaico_tof_handle_t sensor, int32_t offset_um)
{
    ESP_RETURN_ON_FALSE(sensor && offset_um >= -512000 && offset_um <= 511000 && offset_um % 250 == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid distance offset");
    if (sensor->model == SENSOR_VL53L0X) {
        ESP_RETURN_ON_FALSE(!sensor->ranging, ESP_ERR_INVALID_STATE, TAG, "configure offset before continuous ranging");
        return vl53l0x_set_offset_calibration(sensor->l0x, offset_um);
    }
    ESP_RETURN_ON_FALSE(sensor->model == SENSOR_VL53L4CD, ESP_ERR_INVALID_STATE, TAG, "unknown sensor model");
    /* Adjust only the main offset; retain the other factory calibration registers. */
    uint16_t value = (uint16_t)(offset_um / 250) & 0x1FFF;
    return l4cd_status(sensor, "write offset", VL53L4CD_WrWord(&sensor->l4cd, VL53L4CD_RANGE_OFFSET_MM, value));
}

esp_err_t mosaico_tof_start_continuous(mosaico_tof_handle_t sensor)
{
    ESP_RETURN_ON_FALSE(sensor, ESP_ERR_INVALID_ARG, TAG, "null sensor handle");
    if (sensor->ranging) return ESP_OK;
    ESP_RETURN_ON_FALSE(sensor->model == SENSOR_VL53L0X, ESP_ERR_INVALID_STATE, TAG, "sensor is not ranging");
    ESP_RETURN_ON_ERROR(vl53l0x_set_mode(sensor->l0x, VL53L0X_MODE_CONTINUOUS), TAG, "set continuous mode failed");
    ESP_RETURN_ON_ERROR(vl53l0x_set_gpio_config(sensor->l0x, VL53L0X_GPIO_FUNCTION_NEW_MEASURE_READY,
                                             VL53L0X_INTERRUPT_POLARITY_LOW), TAG, "set data-ready interrupt failed");
    ESP_RETURN_ON_ERROR(vl53l0x_start_measurement(sensor->l0x), TAG, "start continuous ranging failed");
    sensor->ranging = true;
    return ESP_OK;
}

esp_err_t mosaico_tof_read_ready(mosaico_tof_handle_t sensor, mosaico_tof_data_t *data, bool *ready)
{
    if (data) memset(data, 0, sizeof(*data));
    if (ready) *ready = false;
    ESP_RETURN_ON_FALSE(sensor && data && ready, ESP_ERR_INVALID_ARG, TAG, "invalid poll arguments");
    ESP_RETURN_ON_FALSE(sensor->ranging, ESP_ERR_INVALID_STATE, TAG, "continuous ranging not started");
    bool available = false;
    if (sensor->model == SENSOR_VL53L0X) {
        ESP_RETURN_ON_ERROR(vl53l0x_get_ready(sensor->l0x, &available), TAG, "read ready flag failed");
        if (!available) return ESP_OK;
        uint32_t observed_ms = (uint32_t)(esp_timer_get_time() / 1000);
        esp_err_t ret = vl53l0x_get_data(sensor->l0x, data);
        if (ret == ESP_OK) ret = vl53l0x_clear_interrupt_mask(sensor->l0x);
        if (ret != ESP_OK) {
            memset(data, 0, sizeof(*data));
            return ret;
        }
        data->timestamp_ms = observed_ms;
        *ready = true;
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(sensor->model == SENSOR_VL53L4CD, ESP_ERR_INVALID_STATE, TAG, "unknown sensor model");
    uint8_t l4_ready = 0;
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "data ready", VL53L4CD_CheckForDataReady(&sensor->l4cd, &l4_ready)), TAG, "read ready flag failed");
    if (!l4_ready) return ESP_OK;
    uint32_t observed_ms = (uint32_t)(esp_timer_get_time() / 1000);
    VL53L4CD_ResultsData_t result = {0};
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "read result", VL53L4CD_GetResult(&sensor->l4cd, &result)), TAG, "read result failed");
    ESP_RETURN_ON_ERROR(l4cd_status(sensor, "clear interrupt", VL53L4CD_ClearInterrupt(&sensor->l4cd)), TAG, "clear interrupt failed");
    data->distance_mm = result.distance_mm;
    data->signal_rate_mcps = result.signal_rate_kcps / 1000.0f;
    data->ambient_rate_mcps = result.ambient_rate_kcps / 1000.0f;
    data->effective_spad_count = result.number_of_spad;
    data->range_status = result.range_status;
    data->valid = result.range_status == 0 && result.number_of_spad != 0;
    data->timestamp_ms = observed_ms;
    *ready = true;
    return ESP_OK;
}

esp_err_t mosaico_tof_read(mosaico_tof_handle_t sensor, mosaico_tof_data_t *data)
{
    if (data) memset(data, 0, sizeof(*data));
    ESP_RETURN_ON_FALSE(sensor && data, ESP_ERR_INVALID_ARG, TAG, "invalid read arguments");
    if (sensor->model == SENSOR_VL53L0X && !sensor->ranging) return vl53l0x_single_measure(sensor->l0x, data);
    const int64_t deadline = esp_timer_get_time() + 500000;
    while (true) {
        bool ready = false;
        ESP_RETURN_ON_ERROR(mosaico_tof_read_ready(sensor, data, &ready), TAG, "read continuous sample failed");
        if (ready) return ESP_OK;
        ESP_RETURN_ON_FALSE(esp_timer_get_time() < deadline, ESP_ERR_TIMEOUT, TAG, "data-ready timeout");
        vTaskDelay(pdMS_TO_TICKS(2) + 1);
    }
}

esp_err_t mosaico_tof_close(mosaico_tof_handle_t sensor)
{
    ESP_RETURN_ON_FALSE(sensor, ESP_ERR_INVALID_ARG, TAG, "null sensor handle");
    if (sensor->ranging && sensor->l4cd.i2c) {
        /* Removal can make the final bus write fail; still release host resources. */
        sensor->l4cd.error = ESP_OK;
        uint8_t status = VL53L4CD_StopRanging(&sensor->l4cd);
        if (status) {
            ESP_LOGW(TAG, "VL53L4CD stop failed during cleanup: %s", esp_err_to_name(sensor->l4cd.error));
        }
        sensor->ranging = false;
    }
    /* The legacy driver stops continuous ranging while the sensor is still awake.
     * Its best-effort destroy also frees host handles after physical removal. */
    if (sensor->l0x) {
        esp_err_t ret = vl53l0x_destroy(sensor->l0x);
        if (ret != ESP_OK) {
            gpio_set_level(sensor->xshut, 0);
            return ret;
        }
        sensor->l0x = NULL;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(sensor->xshut, 0), TAG, "assert XSHUT during cleanup failed");
    if (sensor->l4cd.i2c) {
        ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(sensor->l4cd.i2c), TAG, "remove sensor I2C handle failed");
        sensor->l4cd.i2c = NULL;
    }
    free(sensor);
    return ESP_OK;
}
