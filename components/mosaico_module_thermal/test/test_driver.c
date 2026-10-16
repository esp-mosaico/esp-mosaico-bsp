/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mosaico_module_thermal.h"
#include "MLX90640_API.h"

static int bus_token;
static bool bus_ready = true;
static bool power_fail;
esp_err_t bsp_power_set_codec_3v3(bool enable) { assert(enable); return power_fail ? ESP_FAIL : ESP_OK; }
i2c_master_bus_handle_t bsp_subboard_get_i2c_bus(void) { return bus_ready ? &bus_token : NULL; }

static int64_t now;
static unsigned attached, removed, reads;
static bool nack, ready, invalid_aux, overrun, fail_remove;
static uint16_t control;
static bool continuous, repeat_page;
static unsigned status_reads;
static unsigned transient_overruns;
static uint16_t eeprom[832];

int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(unsigned ticks) { now += ticks * 1000; }

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config, i2c_master_dev_handle_t *out)
{
    (void)config;
    *out = bus;
    ++attached;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    (void)device;
    if (fail_remove) { return ESP_FAIL; }
    ++removed;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t size, int timeout)
{
    (void)device;
    assert(size == 4 && timeout > 0);
    if (nack) { return ESP_FAIL; }
    if (((data[0] << 8) | data[1]) == MLX90640_CTRL_REG) {
        control = (data[2] << 8) | data[3];
    }
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *reg, size_t reg_size, uint8_t *data, size_t size, int timeout)
{
    (void)device;
    assert(reg_size == 2 && timeout > 0);
    ++reads;
    now += 100;
    if (nack) { return ESP_FAIL; }
    unsigned address = (reg[0] << 8) | reg[1];
    for (unsigned i = 0; i < size / 2; ++i) {
        uint16_t value = 0;
        if (address == MLX90640_EEPROM_START_ADDRESS) { value = eeprom[i]; }
        if (address == MLX90640_CTRL_REG) { value = control; }
        if (address == MLX90640_STATUS_REG) { value = ready ? 8 : 0; ready = overrun; }
        if (continuous && address == MLX90640_STATUS_REG) {
            value = status_reads % 2 ? 0 : (8 | (repeat_page ? 0 : (status_reads / 2) % 2));
            if (status_reads % 2 && transient_overruns) {
                value = 8;
                --transient_overruns;
            }
            ++status_reads;
        }
        if (continuous && address == MLX90640_AUX_DATA_START_ADDRESS) {
            if (i == 0 || i == 32) { value = 1000; }
            if (i == 10) { value = 10000; }
            if (i == 42) { value = 0x8000; }
        }
        if (address == MLX90640_AUX_DATA_START_ADDRESS && invalid_aux) { value = 0x7fff; }
        data[2 * i] = value >> 8;
        data[2 * i + 1] = value & 0xff;
    }
    return ESP_OK;
}

int main(void)
{
    mosaico_thermal_config_t config = MOSAICO_THERMAL_DEFAULT_CONFIG();
    mosaico_thermal_handle_t sensor = NULL;
    assert(mosaico_thermal_new(BSP_SUBBOARD_SLOT_COUNT, &config, &sensor) == ESP_ERR_INVALID_ARG);
    bus_ready = false;
    assert(mosaico_thermal_new(BSP_SUBBOARD_SLOT_RIGHT, NULL, &sensor) == ESP_ERR_INVALID_STATE && !sensor);
    bus_ready = true;
    power_fail = true;
    assert(mosaico_thermal_new(BSP_SUBBOARD_SLOT_RIGHT, NULL, &sensor) == ESP_FAIL && !sensor);
    power_fail = false;
    config.emissivity = 0;
    assert(mosaico_thermal_new(BSP_SUBBOARD_SLOT_LEFT, &config, &sensor) == ESP_ERR_INVALID_ARG);
    config.emissivity = 0.95f;
    nack = true;
    assert(mosaico_thermal_new(BSP_SUBBOARD_SLOT_LEFT, &config, &sensor) == ESP_FAIL && !sensor);
    assert(attached == removed);
    nack = false;
    /* An empty EEPROM must fail without hanging in calibration scaling. */
    assert(mosaico_thermal_new(BSP_SUBBOARD_SLOT_LEFT, &config, &sensor) == ESP_ERR_INVALID_RESPONSE && !sensor);
    assert(attached == removed);
    /* Synthetic calibration exercises zero Kta/Kv and signed row corrections. */
    for (unsigned i = 64; i < 832; ++i) { eeprom[i] = 0x10; }
    /* Four defective corners exercise correction at every image boundary. */
    eeprom[64] = eeprom[95] = eeprom[800] = eeprom[831] = 0;
    eeprom[33] = 10000;
    eeprom[34] = 0xffff;
    eeprom[48] = 10000;
    eeprom[49] = 29127;
    eeprom[50] = 8;
    eeprom[51] = 0x0100;
    assert(mosaico_thermal_new(BSP_SUBBOARD_SLOT_LEFT, &config, &sensor) == ESP_OK && sensor);
    mosaico_thermal_frame_t frame, original;
    memset(&frame, 0x5a, sizeof(frame));
    original = frame;
    assert(mosaico_thermal_read(sensor, &frame, 0) == ESP_ERR_INVALID_ARG);
    int64_t started = now;
    assert(mosaico_thermal_read(sensor, &frame, 10) == ESP_ERR_TIMEOUT);
    assert(now - started >= 10000 && now - started < 12000 && reads < 50);
    assert(memcmp(&frame, &original, sizeof(frame)) == 0);
    ready = true;
    invalid_aux = true;
    assert(mosaico_thermal_read(sensor, &frame, 10) == ESP_ERR_INVALID_RESPONSE);
    assert(memcmp(&frame, &original, sizeof(frame)) == 0);
    invalid_aux = false;
    ready = overrun = true;
    assert(mosaico_thermal_read(sensor, &frame, 10) == ESP_ERR_TIMEOUT);
    continuous = true;
    assert(mosaico_thermal_read(sensor, &frame, 100) == ESP_OK);
    assert(status_reads == 4);
    assert(fabsf(frame.ambient_c - 25.111f) < 0.1f);
    assert(fabsf(frame.supply_v - 3.3f) < 0.01f);
    for (unsigned i = 0; i < MOSAICO_THERMAL_PIXEL_COUNT; ++i) {
        assert(isfinite(frame.pixels[i]) && frame.pixels[i] > 20 && frame.pixels[i] < 30);
    }
    assert(frame.pixels[0] == frame.pixels[33]);
    assert(frame.pixels[31] == frame.pixels[62]);
    assert(frame.pixels[736] == frame.pixels[705]);
    assert(frame.pixels[767] == frame.pixels[734]);
    /* A transient overrun discards the partial frame, then acquires both pages. */
    status_reads = 0;
    transient_overruns = 1;
    assert(mosaico_thermal_read(sensor, &frame, 100) == ESP_OK);
    assert(status_reads == 6 && transient_overruns == 0);
    original = frame;
    repeat_page = true;
    assert(mosaico_thermal_read(sensor, &frame, 10) == ESP_ERR_TIMEOUT);
    assert(memcmp(&frame, &original, sizeof(frame)) == 0);
    continuous = false;
    fail_remove = true;
    assert(mosaico_thermal_del(sensor) == ESP_FAIL);
    fail_remove = false;
    assert(mosaico_thermal_del(sensor) == ESP_OK);
    assert(attached == removed);
    nack = fail_remove = true;
    assert(mosaico_thermal_new(BSP_SUBBOARD_SLOT_LEFT, &config, &sensor) == ESP_FAIL && sensor);
    nack = fail_remove = false;
    assert(mosaico_thermal_del(sensor) == ESP_OK);
    assert(attached == removed);
    puts("MLX90640 host fault-path tests passed");
    return 0;
}
