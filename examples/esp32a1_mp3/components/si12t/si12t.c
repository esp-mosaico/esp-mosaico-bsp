/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "si12t.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SI12T_REG_SENSITIVITY_1  0x02U
#define SI12T_REG_CONFIG         0x08U
#define SI12T_REG_CONTROL        0x09U
#define SI12T_REG_REFERENCE_1    0x0AU
#define SI12T_REG_OUTPUT_1       0x10U

#define SI12T_CONTROL_RESET      0x0FU
#define SI12T_CONTROL_RUN        0x07U
/* ILC=01 reports low/medium/high, matching the output decoder below. */
#define SI12T_CONFIG_VALUE       0x2AU
#define SI12T_SENSITIVITY_VALUE  0x11U
#define SI12T_SENSITIVITY_REGS   6U
#define SI12T_VALID_CHANNEL_MASK  0x0FFFU
#define SI12T_PROBE_ATTEMPTS     10U

struct si12t_t {
    i2c_master_dev_handle_t device;
    uint32_t timeout_ms;
};

static const char *TAG = "si12t";

static esp_err_t write_registers(si12t_handle_t handle, uint8_t reg,
                                 const uint8_t *data, size_t data_size)
{
    ESP_RETURN_ON_FALSE(handle && handle->device && data && data_size > 0 && data_size <= 15,
                        ESP_ERR_INVALID_ARG, TAG, "invalid register write");

    uint8_t transaction[16];
    transaction[0] = reg;
    memcpy(&transaction[1], data, data_size);
    return i2c_master_transmit(handle->device, transaction, data_size + 1,
                               (int)handle->timeout_ms);
}

static esp_err_t write_register(si12t_handle_t handle, uint8_t reg, uint8_t value)
{
    return write_registers(handle, reg, &value, sizeof(value));
}

static esp_err_t read_registers(si12t_handle_t handle, uint8_t reg,
                                uint8_t *data, size_t data_size)
{
    ESP_RETURN_ON_FALSE(handle && handle->device && data && data_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid register read");

    /* Si12T requires a short delay between selecting and reading a register. */
    ESP_RETURN_ON_ERROR(i2c_master_transmit(handle->device, &reg, sizeof(reg),
                                            (int)handle->timeout_ms),
                        TAG, "select register 0x%02x failed", reg);
    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_receive(handle->device, data, data_size,
                              (int)handle->timeout_ms);
}

static esp_err_t configure_device(si12t_handle_t handle)
{
    static const uint8_t reference_config[6] = {0};
    static const uint8_t sensitivity[6] = {
        SI12T_SENSITIVITY_VALUE, SI12T_SENSITIVITY_VALUE,
        SI12T_SENSITIVITY_VALUE, SI12T_SENSITIVITY_VALUE,
        SI12T_SENSITIVITY_VALUE, SI12T_SENSITIVITY_VALUE,
    };

    ESP_RETURN_ON_ERROR(write_registers(handle, SI12T_REG_REFERENCE_1,
                                        reference_config, sizeof(reference_config)),
                        TAG, "configure channel references failed");
    ESP_RETURN_ON_ERROR(write_register(handle, SI12T_REG_CONTROL, SI12T_CONTROL_RESET),
                        TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(write_register(handle, SI12T_REG_CONTROL, SI12T_CONTROL_RUN),
                        TAG, "enter run mode failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(write_register(handle, SI12T_REG_CONFIG, SI12T_CONFIG_VALUE),
                        TAG, "configure device failed");
    ESP_RETURN_ON_ERROR(write_registers(handle, SI12T_REG_SENSITIVITY_1,
                                        sensitivity, sizeof(sensitivity)),
                        TAG, "configure sensitivity failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

esp_err_t si12t_create(const si12t_config_t *config, si12t_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(config && config->bus && out_handle && config->address <= 0x7f &&
                            config->clock_speed_hz > 0 && config->timeout_ms > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid configuration");
    *out_handle = NULL;

    esp_err_t ret = ESP_FAIL;
    for (unsigned attempt = 0; attempt < SI12T_PROBE_ATTEMPTS; ++attempt) {
        ret = i2c_master_probe(config->bus, config->address, (int)config->timeout_ms);
        if (ret == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "device 0x%02x did not respond", config->address);

    si12t_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "allocate device failed");

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->address,
        .scl_speed_hz = config->clock_speed_hz,
    };
    ret = i2c_master_bus_add_device(config->bus, &device_config, &handle->device);
    if (ret != ESP_OK) {
        free(handle);
        return ret;
    }
    handle->timeout_ms = config->timeout_ms;

    ret = configure_device(handle);
    if (ret == ESP_OK) {
        uint16_t ignored;
        ret = si12t_read_pressed(handle, &ignored);
    }
    if (ret != ESP_OK) {
        (void)i2c_master_bus_rm_device(handle->device);
        free(handle);
        return ret;
    }

    *out_handle = handle;
    ESP_LOGI(TAG, "ready on subboard I2C: address=0x%02x", config->address);
    return ESP_OK;
}

esp_err_t si12t_read_pressed(si12t_handle_t handle, uint16_t *pressed_mask)
{
    ESP_RETURN_ON_FALSE(handle && pressed_mask, ESP_ERR_INVALID_ARG, TAG,
                        "invalid pressed-mask output");

    uint8_t raw[3];
    ESP_RETURN_ON_ERROR(read_registers(handle, SI12T_REG_OUTPUT_1, raw, sizeof(raw)),
                        TAG, "read channel state failed");

    uint16_t result = 0;
    for (unsigned channel = 0; channel < SI12T_CHANNEL_COUNT; ++channel) {
        const unsigned shift = (channel % 4U) * 2U;
        if (((raw[channel / 4U] >> shift) & 0x03U) != 0) {
            result |= (uint16_t)(1U << channel);
        }
    }
    *pressed_mask = result;
    return ESP_OK;
}

esp_err_t si12t_set_max_sensitivity(si12t_handle_t handle, uint16_t channel_mask)
{
    return si12t_set_sensitivity(handle, channel_mask, 0x00U);
}

esp_err_t si12t_set_sensitivity(si12t_handle_t handle, uint16_t channel_mask, uint8_t value)
{
    ESP_RETURN_ON_FALSE(value <= 0x0fU, ESP_ERR_INVALID_ARG, TAG, "invalid SEN nibble");
    ESP_RETURN_ON_FALSE(handle && (channel_mask & SI12T_VALID_CHANNEL_MASK),
                        ESP_ERR_INVALID_ARG, TAG, "invalid sensitivity channel mask");
    ESP_RETURN_ON_FALSE((channel_mask & ~SI12T_VALID_CHANNEL_MASK) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "sensitivity mask exceeds 12 channels");

    uint8_t sensitivity[SI12T_SENSITIVITY_REGS];
    ESP_RETURN_ON_ERROR(read_registers(handle, SI12T_REG_SENSITIVITY_1,
                                       sensitivity, sizeof(sensitivity)),
                        TAG, "read sensitivity registers failed");

    for (unsigned channel = 0; channel < SI12T_CHANNEL_COUNT; ++channel) {
        if ((channel_mask & (1U << channel)) == 0) {
            continue;
        }
        const unsigned register_index = channel / 2U;
        if ((channel & 1U) == 0) {
            sensitivity[register_index] = (sensitivity[register_index] & 0xF0U) | value;
        } else {
            sensitivity[register_index] = (sensitivity[register_index] & 0x0FU) | (value << 4);
        }
    }

    ESP_RETURN_ON_ERROR(write_registers(handle, SI12T_REG_SENSITIVITY_1,
                                        sensitivity, sizeof(sensitivity)),
                        TAG, "write sensitivity failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t verification[SI12T_SENSITIVITY_REGS];
    ESP_RETURN_ON_ERROR(read_registers(handle, SI12T_REG_SENSITIVITY_1,
                                       verification, sizeof(verification)),
                        TAG, "verify sensitivity registers failed");
    ESP_RETURN_ON_FALSE(memcmp(sensitivity, verification, sizeof(sensitivity)) == 0,
                        ESP_ERR_INVALID_RESPONSE, TAG,
                        "sensitivity register verification failed");
    ESP_LOGI(TAG, "sensitivity verified: channel mask=0x%03x SEN=0x%01x",
             channel_mask, value);
    return ESP_OK;
}

esp_err_t si12t_delete(si12t_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "device is null");
    const esp_err_t ret = i2c_master_bus_rm_device(handle->device);
    free(handle);
    return ret;
}
