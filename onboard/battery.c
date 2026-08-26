/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp_mosaico.h"
#include "bq27220.h"
#include "esp_check.h"
#include "esp_log.h"
#include "i2c_bus.h"

#define BATTERY_KELVIN_OFFSET    273.15f

static const char *TAG = "S31-Mosaico-Battery";
static bq27220_handle_t s_gauge;
/* This wrapper borrows the BSP-owned bus and remains valid for the BSP lifetime. */
static i2c_bus_handle_t s_battery_bus;

/* Interim fixed-EDV profile characterized on two 80 mAh cells at 25 C. */
static const parameter_cedv_t s_default_cedv = {
    .full_charge_cap = 80,
    .design_cap = 80,
    .reserve_cap = 0,
    .near_full = 5,
    .self_discharge_rate = 20,
    .EDV0 = 3000,
    .EDV1 = 3410,
    .EDV2 = 3530,
    .EMF = 3670,
    .C0 = 115,
    .R0 = 968,
    .T0 = 4547,
    .R1 = 4764,
    .TC = 11,
    .C1 = 0,
    .DOD0 = 4147,
    .DOD10 = 4002,
    .DOD20 = 3969,
    .DOD30 = 3938,
    .DOD40 = 3880,
    .DOD50 = 3824,
    .DOD60 = 3794,
    .DOD70 = 3753,
    .DOD80 = 3677,
    .DOD90 = 3574,
    .DOD100 = 3490,
};

static const gauging_config_t s_default_gauging = {
    .CCT = true,
    .SC = true,
    .FCC_LIM = true,
    .FC_FOR_VDQ = true,
    .IGNORE_SD = true,
};

static esp_err_t battery_attach(void)
{
    if (s_gauge) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "shared I2C init failed");

    if (!s_battery_bus) {
        const i2c_config_t bus_config = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = BSP_I2C_SDA,
            .scl_io_num = BSP_I2C_SCL,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = BSP_BATTERY_I2C_SPEED_HZ,
        };
        s_battery_bus = i2c_bus_create(BSP_I2C_PORT, &bus_config);
        ESP_RETURN_ON_FALSE(s_battery_bus, ESP_FAIL, TAG, "wrap shared I2C bus failed");
    }

    const bq27220_config_t config = {
        .i2c_bus = s_battery_bus,
        .cfg = &s_default_gauging,
        .cedv = &s_default_cedv,
    };
    s_gauge = bq27220_create(&config);
    ESP_RETURN_ON_FALSE(s_gauge, ESP_FAIL, TAG, "attach BQ27220 0x%02X failed", BSP_BATTERY_I2C_ADDR);
    const esp_err_t seal_ret = bq27220_seal(s_gauge);
    if (seal_ret != ESP_OK) {
        ESP_LOGE(TAG, "seal BQ27220 after initialization failed: %s", esp_err_to_name(seal_ret));
        bq27220_delete(s_gauge);
        s_gauge = NULL;
        return seal_ret;
    }

    const uint16_t voltage_mv = bq27220_get_voltage(s_gauge);
    ESP_LOGI(TAG, "BQ27220 online: address=0x%02X SDA=%d SCL=%d %u mV", BSP_BATTERY_I2C_ADDR,
             BSP_I2C_SDA, BSP_I2C_SCL, voltage_mv);
    ESP_LOGI(TAG, "BQ27220 default profile active: %u mAh EDV=%u/%u/%u mV", s_default_cedv.design_cap,
             s_default_cedv.EDV0, s_default_cedv.EDV1, s_default_cedv.EDV2);
    return ESP_OK;
}

esp_err_t bsp_battery_init(void)
{
    if (s_gauge) {
        return ESP_OK;
    }
    return battery_attach();
}

esp_err_t bsp_battery_deinit(void)
{
    if (!s_gauge) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bq27220_delete(s_gauge), TAG, "detach BQ27220 failed");
    s_gauge = NULL;
    return ESP_OK;
}

esp_err_t bsp_battery_read_basic(int16_t *voltage_mv, int16_t *current_ma, float *temperature_c)
{
    ESP_RETURN_ON_FALSE(s_gauge, ESP_ERR_INVALID_STATE, TAG, "battery gauge is not initialized");
    ESP_RETURN_ON_FALSE(voltage_mv && current_ma && temperature_c, ESP_ERR_INVALID_ARG, TAG,
                        "battery output is null");

    *voltage_mv = (int16_t)bq27220_get_voltage(s_gauge);
    *current_ma = bq27220_get_current(s_gauge);
    const uint16_t temperature = bq27220_get_temperature(s_gauge);
    *temperature_c = temperature / 10.0f - BATTERY_KELVIN_OFFSET;
    return ESP_OK;
}

esp_err_t bsp_battery_read(bsp_battery_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "battery status output is null");
    ESP_RETURN_ON_ERROR(bsp_battery_read_basic(&status->voltage_mv, &status->current_ma,
                                               &status->temperature_c), TAG,
                        "read battery basics failed");

    status->average_current_ma = bq27220_get_avgcurrent(s_gauge);
    status->average_power_mw = bq27220_get_average_power(s_gauge);
    status->max_load_current_ma = bq27220_get_maxload_current(s_gauge);
    status->standby_current_ma = bq27220_get_standby_current(s_gauge);
    battery_status_t battery_status = {0};
    ESP_RETURN_ON_ERROR(bq27220_get_battery_status(s_gauge, &battery_status), TAG, "read status failed");
    status->status_flags = battery_status.full;
    status->remaining_capacity_mah = bq27220_get_remaining_capacity(s_gauge);
    status->full_charge_capacity_mah = bq27220_get_full_charge_capacity(s_gauge);
    status->cycle_count = bq27220_get_cycle_count(s_gauge);
    status->time_to_empty_min = bq27220_get_time_to_empty(s_gauge);
    status->time_to_full_min = bq27220_get_time_to_full(s_gauge);
    status->state_of_charge = (uint8_t)bq27220_get_state_of_charge(s_gauge);
    status->state_of_health = (uint8_t)bq27220_get_state_of_health(s_gauge);
    return ESP_OK;
}
