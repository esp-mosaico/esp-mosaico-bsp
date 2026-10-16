/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPDX-License-Identifier: Apache-2.0 */
#include "MLX90640_I2C_Driver.h"
#include "esp_timer.h"

static int transfer_timeout(mlx90640_io_t *io)
{
    int64_t remaining = io->deadline_us - esp_timer_get_time();
    if (remaining <= 0) {
        io->error = ESP_ERR_TIMEOUT;
        return 0;
    }
    return remaining > 100000 ? 100 : (int)((remaining + 999) / 1000);
}

int MLX90640_I2CRead(mlx90640_io_t *io, uint16_t address, uint16_t words, uint16_t *data)
{
    int timeout = transfer_timeout(io);
    if (!timeout) {
        return -MLX90640_I2C_NACK_ERROR;
    }
    uint8_t reg[] = {address >> 8, address & 0xff};
    io->error = i2c_master_transmit_receive(io->device, reg, sizeof(reg), (uint8_t *)data, words * 2, timeout);
    if (io->error != ESP_OK) {
        return -MLX90640_I2C_NACK_ERROR;
    }
    /* Decode in place, independently of host endianness. */
    uint8_t *raw = (uint8_t *)data;
    for (unsigned i = 0; i < words; ++i) {
        data[i] = ((uint16_t)raw[2 * i] << 8) | raw[2 * i + 1];
    }
    return 0;
}

int MLX90640_I2CWrite(mlx90640_io_t *io, uint16_t address, uint16_t value)
{
    int timeout = transfer_timeout(io);
    if (!timeout) {
        return -MLX90640_I2C_NACK_ERROR;
    }
    uint8_t raw[] = {address >> 8, address & 0xff, value >> 8, value & 0xff};
    io->error = i2c_master_transmit(io->device, raw, sizeof(raw), timeout);
    if (io->error != ESP_OK) {
        return -MLX90640_I2C_NACK_ERROR;
    }
    /* Status bits change autonomously and cannot be verified by equality. */
    if (address != MLX90640_STATUS_REG) {
        uint16_t actual = 0;
        int ret = MLX90640_I2CRead(io, address, 1, &actual);
        if (ret) {
            return ret;
        }
        if (actual != value) {
            io->error = ESP_ERR_INVALID_RESPONSE;
            return -MLX90640_I2C_WRITE_ERROR;
        }
    }
    return 0;
}

int MLX90640_I2CGeneralReset(void)
{
    /* Never broadcast a reset on a caller-owned shared bus. */
    return -MLX90640_I2C_NACK_ERROR;
}
