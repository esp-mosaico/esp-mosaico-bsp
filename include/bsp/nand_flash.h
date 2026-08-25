/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "spi_nand_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_NAND_FLASH_DEFAULT_CLOCK_HZ        (40 * 1000 * 1000)
#define BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE      10
#define BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ (4 * 1024)

typedef struct {
    int clock_speed_hz;
    int queue_size;
    int max_transfer_sz;
    uint8_t gc_factor;
    spi_nand_flash_io_mode_t io_mode;
    uint8_t flags;
} bsp_nand_flash_config_t;

#if CONFIG_BSP_NAND_USE_QIO
#define BSP_NAND_FLASH_DEFAULT_IO_MODE SPI_NAND_IO_MODE_QIO
#else
#define BSP_NAND_FLASH_DEFAULT_IO_MODE SPI_NAND_IO_MODE_SIO
#endif

#define BSP_NAND_FLASH_DEFAULT_CONFIG() {                     \
    .clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ,        \
    .queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE,          \
    .max_transfer_sz = BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ,\
    .gc_factor = 0,                                           \
    .io_mode = BSP_NAND_FLASH_DEFAULT_IO_MODE,                \
    .flags = SPI_DEVICE_HALFDUPLEX,                           \
}

esp_err_t bsp_nand_flash_init(const bsp_nand_flash_config_t *config,
                              spi_nand_flash_device_t **out_handle);
esp_err_t bsp_nand_flash_deinit(void);
spi_nand_flash_device_t *bsp_nand_flash_get_handle(void);

/** Sync NAND and leave the bus idle (CS# high standby). No-op if not initialized. */
esp_err_t bsp_nand_flash_enter_power_save(void);

/** Clear the power-save marker after `bsp_nand_flash_enter_power_save()`. */
esp_err_t bsp_nand_flash_exit_power_save(void);

#ifdef __cplusplus
}
#endif
