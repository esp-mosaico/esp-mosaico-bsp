/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bmm150_common.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "S31-Mosaico-Sensor";

#define BMM150_DATA_READY_TIMEOUT_MS 200
#define BMM150_DATA_READY_POLL_MS    5

static bmi270_handle_t s_bmi_handle;
/* Failed creation may retain a handle solely for cleanup. */
static bool s_bmi_initialized;
static bsp_imu_config_t s_bmi_config = BSP_IMU_CONFIG_DEFAULT();
static bool s_bmi_started;

typedef struct {
    uint8_t address;
    struct bmm150_dev sensor;
    bool initialized;
} bmm150_context_t;

static bmm150_context_t s_magnetometers[BSP_MAGNETOMETER_NUM] = {
    [BSP_MAGNETOMETER_0].address = BSP_BMM150_ADDR_0,
    [BSP_MAGNETOMETER_1].address = BSP_BMM150_ADDR_1,
};

esp_err_t bsp_imu_init(void)
{
    ESP_RETURN_ON_FALSE(!s_bmi_handle, ESP_ERR_INVALID_STATE, TAG, "BMI270 is already initialized");
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 rail failed");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "shared I2C init failed");
    ESP_RETURN_ON_ERROR(
        bmi270_sensor_create_from_master_bus_with_address(bsp_i2c_get_handle(), BSP_IMU_I2C_ADDR,
                                                          &s_bmi_handle, bmi270_config_file,
                                                          BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE),
        TAG, "create BMI270 failed");
    s_bmi_initialized = true;
    ESP_LOGI(TAG, "BMI270 initialized: address=0x%02X shared_INT=%d", BSP_IMU_I2C_ADDR, BSP_SENSOR_INT);
    return ESP_OK;
}

esp_err_t bsp_imu_start(const bsp_imu_config_t *config)
{
    ESP_RETURN_ON_FALSE(s_bmi_initialized, ESP_ERR_INVALID_STATE, TAG, "BMI270 is not initialized");
    const bsp_imu_config_t active = config ? *config : (bsp_imu_config_t)BSP_IMU_CONFIG_DEFAULT();
    struct bmi2_sens_config sensors[2] = {{.type = BMI2_ACCEL}, {.type = BMI2_GYRO}};
    int8_t result = bmi2_get_sensor_config(sensors, 2, s_bmi_handle);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG, "get BMI270 configuration failed: %d", result);
    sensors[0].cfg.acc.odr = active.accel_odr;
    sensors[0].cfg.acc.range = active.accel_range;
    sensors[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    sensors[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    sensors[1].cfg.gyr.odr = active.gyro_odr;
    sensors[1].cfg.gyr.range = active.gyro_range;
    sensors[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    sensors[1].cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
    sensors[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
    result = bmi2_set_sensor_config(sensors, 2, s_bmi_handle);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG, "set BMI270 configuration failed: %d", result);
    uint8_t sensor_list[] = {BMI2_ACCEL, BMI2_GYRO};
    result = bmi2_sensor_enable(sensor_list, 2, s_bmi_handle);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG, "enable BMI270 sensors failed: %d", result);
    s_bmi_config = active;
    s_bmi_started = true;
    return ESP_OK;
}

esp_err_t bsp_imu_stop(void)
{
    ESP_RETURN_ON_FALSE(s_bmi_initialized, ESP_ERR_INVALID_STATE, TAG, "BMI270 is not initialized");
    uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    int8_t result = bmi2_sensor_disable(sensors, 2, s_bmi_handle);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG, "disable BMI270 sensors failed: %d", result);
    s_bmi_started = false;
    return ESP_OK;
}

esp_err_t bsp_imu_deinit(void)
{
    if (!s_bmi_handle) {
        return ESP_OK;
    }
    if (s_bmi_started) {
        ESP_RETURN_ON_ERROR(bsp_imu_stop(), TAG, "stop BMI270 failed");
    }
    ESP_RETURN_ON_ERROR(bmi270_sensor_del(&s_bmi_handle), TAG, "delete BMI270 failed");
    s_bmi_initialized = false;
    return ESP_OK;
}

static esp_err_t read_imu(struct bmi2_sens_data *data)
{
    ESP_RETURN_ON_FALSE(s_bmi_initialized && s_bmi_started, ESP_ERR_INVALID_STATE, TAG, "BMI270 is not running");
    int8_t result = bmi2_get_sensor_data(data, s_bmi_handle);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG, "read BMI270 data failed: %d", result);
    return ESP_OK;
}

esp_err_t bsp_imu_get_accel(float *x, float *y, float *z)
{
    ESP_RETURN_ON_FALSE(x && y && z, ESP_ERR_INVALID_ARG, TAG, "acceleration output is null");
    struct bmi2_sens_data data = {0};
    ESP_RETURN_ON_ERROR(read_imu(&data), TAG, "read acceleration failed");
    const float full_scale[] = {2.0f, 4.0f, 8.0f, 16.0f};
    ESP_RETURN_ON_FALSE(s_bmi_config.accel_range <= BMI2_ACC_RANGE_16G, ESP_ERR_INVALID_STATE, TAG,
                        "invalid cached accelerometer range");
    const float scale = full_scale[s_bmi_config.accel_range] / 32768.0f;
    *x = data.acc.x * scale;
    *y = data.acc.y * scale;
    *z = data.acc.z * scale;
    return ESP_OK;
}

esp_err_t bsp_imu_get_gyro(float *x, float *y, float *z)
{
    ESP_RETURN_ON_FALSE(x && y && z, ESP_ERR_INVALID_ARG, TAG, "gyroscope output is null");
    struct bmi2_sens_data data = {0};
    ESP_RETURN_ON_ERROR(read_imu(&data), TAG, "read gyroscope failed");
    const float full_scale[] = {2000.0f, 1000.0f, 500.0f, 250.0f, 125.0f};
    ESP_RETURN_ON_FALSE(s_bmi_config.gyro_range <= BMI2_GYR_RANGE_125, ESP_ERR_INVALID_STATE, TAG,
                        "invalid cached gyroscope range");
    const float scale = full_scale[s_bmi_config.gyro_range] / 32768.0f;
    *x = data.gyr.x * scale;
    *y = data.gyr.y * scale;
    *z = data.gyr.z * scale;
    return ESP_OK;
}

struct bmi2_dev *bsp_imu_get_handle(void) { return s_bmi_initialized ? bmi270_get_dev(s_bmi_handle) : NULL; }

/* Until the first conversion lands the data registers read back as zero, which
 * compensation turns into an overflow sample. */
static esp_err_t magnetometer_wait_data_ready(struct bmm150_dev *sensor)
{
    for (int waited_ms = 0; waited_ms <= BMM150_DATA_READY_TIMEOUT_MS; waited_ms += BMM150_DATA_READY_POLL_MS) {
        uint8_t status = 0;
        if (bmm150_get_regs(BMM150_REG_DATA_READY_STATUS, &status, 1, sensor) != BMM150_OK) {
            return ESP_FAIL;
        }
        if (status & BMM150_DRDY_STATUS_MSK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(BMM150_DATA_READY_POLL_MS));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t bsp_magnetometer_init(bsp_magnetometer_t id)
{
    ESP_RETURN_ON_FALSE((unsigned)id < BSP_MAGNETOMETER_NUM, ESP_ERR_INVALID_ARG, TAG, "invalid BMM150 index: %d", id);
    bmm150_context_t *context = &s_magnetometers[id];
    if (context->initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 rail failed");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "shared I2C init failed");
    /* Retry cleanup if a previous initialization could not release the device. */
    ESP_RETURN_ON_ERROR(bsp_magnetometer_deinit(id), TAG, "clean up BMM150[%d] failed", id);
    ESP_RETURN_ON_ERROR(bmm150_interface_init_from_master_bus(&context->sensor, bsp_i2c_get_handle(),
                                                             context->address, 400000),
                        TAG, "add BMM150[%d] I2C device failed", id);
    int8_t result = bmm150_init(&context->sensor);
    if (result != BMM150_OK) {
        ESP_LOGE(TAG, "BMM150[%d] initialization failed: address=0x%02X result=%d", id, context->address, result);
        ESP_RETURN_ON_ERROR(bsp_magnetometer_deinit(id), TAG, "clean up BMM150[%d] failed", id);
        return ESP_FAIL;
    }
    struct bmm150_settings settings = {
        .pwr_mode = BMM150_POWERMODE_NORMAL,
        .preset_mode = BMM150_PRESETMODE_REGULAR,
    };
    result = bmm150_set_op_mode(&settings, &context->sensor);
    if (result == BMM150_OK) {
        result = bmm150_set_presetmode(&settings, &context->sensor);
    }
    if (result != BMM150_OK) {
        ESP_LOGE(TAG, "BMM150[%d] configuration failed: %d", id, result);
        ESP_RETURN_ON_ERROR(bsp_magnetometer_deinit(id), TAG, "clean up BMM150[%d] failed", id);
        return ESP_FAIL;
    }
    const esp_err_t ready = magnetometer_wait_data_ready(&context->sensor);
    if (ready != ESP_OK) {
        ESP_LOGW(TAG, "BMM150[%d] first sample not ready: %s", id, esp_err_to_name(ready));
    }
    context->initialized = true;
    ESP_LOGI(TAG, "BMM150[%d] initialized: address=0x%02X shared_INT=%d", id, context->address, BSP_SENSOR_INT);
    return ESP_OK;
}

esp_err_t bsp_magnetometer_init_all(void)
{
    for (int id = 0; id < BSP_MAGNETOMETER_NUM; ++id) {
        ESP_RETURN_ON_ERROR(bsp_magnetometer_init(id), TAG, "initialize BMM150[%d] failed", id);
    }
    return ESP_OK;
}

esp_err_t bsp_magnetometer_deinit(bsp_magnetometer_t id)
{
    ESP_RETURN_ON_FALSE((unsigned)id < BSP_MAGNETOMETER_NUM, ESP_ERR_INVALID_ARG, TAG, "invalid BMM150 index: %d", id);
    bmm150_context_t *context = &s_magnetometers[id];
    if (!context->sensor.intf_ptr) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bmm150_interface_deinit_device(&context->sensor), TAG, "remove BMM150[%d] failed", id);
    context->initialized = false;
    return ESP_OK;
}

esp_err_t bsp_magnetometer_read(bsp_magnetometer_t id, struct bmm150_mag_data *data)
{
    ESP_RETURN_ON_FALSE((unsigned)id < BSP_MAGNETOMETER_NUM && data, ESP_ERR_INVALID_ARG, TAG,
                        "invalid BMM150 read arguments");
    ESP_RETURN_ON_FALSE(s_magnetometers[id].initialized, ESP_ERR_INVALID_STATE, TAG,
                        "BMM150[%d] is not initialized", id);
    int8_t result = bmm150_read_mag_data(data, &s_magnetometers[id].sensor);
    ESP_RETURN_ON_FALSE(result == BMM150_OK, ESP_FAIL, TAG, "read BMM150[%d] failed: %d", id, result);
    return ESP_OK;
}

struct bmm150_dev *bsp_magnetometer_get_handle(bsp_magnetometer_t id)
{
    return (unsigned)id < BSP_MAGNETOMETER_NUM && s_magnetometers[id].initialized ? &s_magnetometers[id].sensor : NULL;
}
