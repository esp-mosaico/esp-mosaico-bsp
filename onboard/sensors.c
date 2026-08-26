/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "S31-Mosaico-Sensor";

#define BMM150_DATA_READY_TIMEOUT_MS 200
#define BMM150_DATA_READY_POLL_MS    5

static struct bmi2_dev s_bmi_device;
static struct bmi2_dev *s_bmi_handle;
static i2c_master_dev_handle_t s_bmi_i2c_device;
static bsp_imu_config_t s_bmi_config = BSP_IMU_CONFIG_DEFAULT();
static bool s_bmi_started;

typedef struct {
    uint8_t address;
    i2c_master_dev_handle_t i2c_device;
    struct bmm150_dev sensor;
    bool initialized;
} bmm150_context_t;

static bmm150_context_t s_magnetometers[BSP_MAGNETOMETER_NUM] = {
    [BSP_MAGNETOMETER_0].address = BSP_BMM150_ADDR_0,
    [BSP_MAGNETOMETER_1].address = BSP_BMM150_ADDR_1,
};

static BMI2_INTF_RETURN_TYPE bmi_i2c_read(uint8_t reg, uint8_t *data, uint32_t length, void *interface)
{
    i2c_master_dev_handle_t device = *(i2c_master_dev_handle_t *)interface;
    return i2c_master_transmit_receive(device, &reg, 1, data, length, -1) == ESP_OK ? BMI2_INTF_RET_SUCCESS : -1;
}

static BMI2_INTF_RETURN_TYPE bmi_i2c_write(uint8_t reg, const uint8_t *data, uint32_t length, void *interface)
{
    if (length > 32) {
        return -1;
    }
    uint8_t buffer[33] = {reg};
    memcpy(&buffer[1], data, length);
    i2c_master_dev_handle_t device = *(i2c_master_dev_handle_t *)interface;
    return i2c_master_transmit(device, buffer, length + 1, -1) == ESP_OK ? BMI2_INTF_RET_SUCCESS : -1;
}

static void sensor_delay_us(uint32_t period, void *interface)
{
    (void)interface;
    esp_rom_delay_us(period);
}

esp_err_t bsp_imu_init(void)
{
    ESP_RETURN_ON_FALSE(!s_bmi_handle, ESP_ERR_INVALID_STATE, TAG, "BMI270 is already initialized");
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 rail failed");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "shared I2C init failed");
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_IMU_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &s_bmi_i2c_device), TAG,
                        "add BMI270 I2C device failed");
    memset(&s_bmi_device, 0, sizeof(s_bmi_device));
    s_bmi_device.intf = BMI2_I2C_INTF;
    s_bmi_device.intf_ptr = &s_bmi_i2c_device;
    s_bmi_device.read = bmi_i2c_read;
    s_bmi_device.write = bmi_i2c_write;
    s_bmi_device.delay_us = sensor_delay_us;
    s_bmi_device.read_write_len = 32;
    int8_t result = bmi270_init(&s_bmi_device);
    if (result != BMI2_OK) {
        ESP_LOGE(TAG, "BMI270 initialization failed: sensor_result=%d load_status=0x%02X",
                 result, s_bmi_device.load_status);
        i2c_master_bus_rm_device(s_bmi_i2c_device);
        s_bmi_i2c_device = NULL;
        return ESP_FAIL;
    }
    s_bmi_handle = &s_bmi_device;
    ESP_LOGI(TAG, "BMI270 initialized: address=0x%02X shared_INT=%d", BSP_IMU_I2C_ADDR, BSP_SENSOR_INT);
    return ESP_OK;
}

esp_err_t bsp_imu_start(const bsp_imu_config_t *config)
{
    ESP_RETURN_ON_FALSE(s_bmi_handle, ESP_ERR_INVALID_STATE, TAG, "BMI270 is not initialized");
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
    ESP_RETURN_ON_FALSE(s_bmi_handle, ESP_ERR_INVALID_STATE, TAG, "BMI270 is not initialized");
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
        bsp_imu_stop();
    }
    ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(s_bmi_i2c_device), TAG, "remove BMI270 I2C device failed");
    s_bmi_i2c_device = NULL;
    s_bmi_handle = NULL;
    return ESP_OK;
}

static esp_err_t read_imu(struct bmi2_sens_data *data)
{
    ESP_RETURN_ON_FALSE(s_bmi_handle && s_bmi_started, ESP_ERR_INVALID_STATE, TAG, "BMI270 is not running");
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

struct bmi2_dev *bsp_imu_get_handle(void) { return s_bmi_handle; }

static BMM150_INTF_RET_TYPE bmm_i2c_read(uint8_t reg, uint8_t *data, uint32_t length, void *interface)
{
    bmm150_context_t *context = interface;
    return i2c_master_transmit_receive(context->i2c_device, &reg, 1, data, length, -1) == ESP_OK ?
           BMM150_INTF_RET_SUCCESS : -1;
}

static BMM150_INTF_RET_TYPE bmm_i2c_write(uint8_t reg, const uint8_t *data, uint32_t length, void *interface)
{
    if (length > 32) {
        return -1;
    }
    uint8_t buffer[33] = {reg};
    memcpy(&buffer[1], data, length);
    bmm150_context_t *context = interface;
    return i2c_master_transmit(context->i2c_device, buffer, length + 1, -1) == ESP_OK ?
           BMM150_INTF_RET_SUCCESS : -1;
}

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
    ESP_RETURN_ON_FALSE(id < BSP_MAGNETOMETER_NUM, ESP_ERR_INVALID_ARG, TAG, "invalid BMM150 index: %d", id);
    bmm150_context_t *context = &s_magnetometers[id];
    if (context->initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "enable VCC_3V3 rail failed");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "shared I2C init failed");
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = context->address,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &context->i2c_device), TAG,
                        "add BMM150[%d] I2C device failed", id);
    context->sensor.intf = BMM150_I2C_INTF;
    context->sensor.intf_ptr = context;
    context->sensor.read = bmm_i2c_read;
    context->sensor.write = bmm_i2c_write;
    context->sensor.delay_us = sensor_delay_us;
    int8_t result = bmm150_init(&context->sensor);
    if (result != BMM150_OK) {
        i2c_master_bus_rm_device(context->i2c_device);
        context->i2c_device = NULL;
        ESP_LOGE(TAG, "BMM150[%d] initialization failed: address=0x%02X result=%d", id, context->address, result);
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
        i2c_master_bus_rm_device(context->i2c_device);
        context->i2c_device = NULL;
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
    ESP_RETURN_ON_FALSE(id < BSP_MAGNETOMETER_NUM, ESP_ERR_INVALID_ARG, TAG, "invalid BMM150 index: %d", id);
    bmm150_context_t *context = &s_magnetometers[id];
    if (!context->initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(context->i2c_device), TAG, "remove BMM150[%d] failed", id);
    context->i2c_device = NULL;
    context->initialized = false;
    return ESP_OK;
}

esp_err_t bsp_magnetometer_read(bsp_magnetometer_t id, struct bmm150_mag_data *data)
{
    ESP_RETURN_ON_FALSE(id < BSP_MAGNETOMETER_NUM && data, ESP_ERR_INVALID_ARG, TAG,
                        "invalid BMM150 read arguments");
    ESP_RETURN_ON_FALSE(s_magnetometers[id].initialized, ESP_ERR_INVALID_STATE, TAG,
                        "BMM150[%d] is not initialized", id);
    int8_t result = bmm150_read_mag_data(data, &s_magnetometers[id].sensor);
    ESP_RETURN_ON_FALSE(result == BMM150_OK, ESP_FAIL, TAG, "read BMM150[%d] failed: %d", id, result);
    return ESP_OK;
}

struct bmm150_dev *bsp_magnetometer_get_handle(bsp_magnetometer_t id)
{
    return id < BSP_MAGNETOMETER_NUM && s_magnetometers[id].initialized ? &s_magnetometers[id].sensor : NULL;
}
