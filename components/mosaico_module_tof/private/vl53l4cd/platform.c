/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "platform.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Latch the first bus error: do not repeat failed transfers in ULD polling loops. */
static uint8_t read_register(Dev_t dev, uint16_t reg, uint8_t *data, size_t size)
{
    memset(data, 0, size);
    if (dev->error == ESP_OK) {
        const uint8_t address[] = {reg >> 8, reg & 0xFF};
        dev->error = i2c_master_transmit_receive(dev->i2c, address, sizeof(address), data, size, 100);
    }
    return dev->error == ESP_OK ? 0 : 1;
}

static uint8_t write_register(Dev_t dev, uint16_t reg, const uint8_t *data, size_t size)
{
    if (dev->error == ESP_OK) {
        uint8_t buffer[6] = {reg >> 8, reg & 0xFF};
        memcpy(buffer + 2, data, size);
        dev->error = i2c_master_transmit(dev->i2c, buffer, size + 2, 100);
    }
    return dev->error == ESP_OK ? 0 : 1;
}

uint8_t VL53L4CD_RdByte(Dev_t dev, uint16_t reg, uint8_t *value)
{
    return read_register(dev, reg, value, 1);
}

uint8_t VL53L4CD_RdWord(Dev_t dev, uint16_t reg, uint16_t *value)
{
    uint8_t bytes[2];
    uint8_t status = read_register(dev, reg, bytes, sizeof(bytes));
    *value = ((uint16_t)bytes[0] << 8) | bytes[1];
    return status;
}

uint8_t VL53L4CD_RdDWord(Dev_t dev, uint16_t reg, uint32_t *value)
{
    uint8_t bytes[4];
    uint8_t status = read_register(dev, reg, bytes, sizeof(bytes));
    *value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
             ((uint32_t)bytes[2] << 8) | bytes[3];
    return status;
}

uint8_t VL53L4CD_WrByte(Dev_t dev, uint16_t reg, uint8_t value)
{
    return write_register(dev, reg, &value, 1);
}

uint8_t VL53L4CD_WrWord(Dev_t dev, uint16_t reg, uint16_t value)
{
    const uint8_t bytes[] = {value >> 8, value & 0xFF};
    return write_register(dev, reg, bytes, sizeof(bytes));
}

uint8_t VL53L4CD_WrDWord(Dev_t dev, uint16_t reg, uint32_t value)
{
    const uint8_t bytes[] = {value >> 24, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF};
    return write_register(dev, reg, bytes, sizeof(bytes));
}

uint8_t VL53L4CD_WaitMs(Dev_t dev, uint32_t ms)
{
    (void)dev;
    if (ms) {
        vTaskDelay(pdMS_TO_TICKS(ms) + 1);
    }
    return 0;
}
