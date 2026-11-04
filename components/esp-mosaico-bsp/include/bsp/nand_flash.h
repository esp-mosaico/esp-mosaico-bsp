/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "spi_nand_flash.h"
#include "esp_blockdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Kconfig selection supplies the default; callers can override it below. */
#ifdef CONFIG_BSP_NAND_FLASH_CLOCK_40M
#define BSP_NAND_FLASH_DEFAULT_CLOCK_HZ        (40 * 1000 * 1000)
#else
#define BSP_NAND_FLASH_DEFAULT_CLOCK_HZ        (80 * 1000 * 1000)
#endif
#define BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE      10
#define BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ (4 * 1024)

typedef struct {
    int clock_speed_hz; /**< Working clock in Hz; zero uses the Kconfig default. */
    int queue_size;
    int max_transfer_sz;
    uint8_t gc_factor;
    spi_nand_flash_io_mode_t io_mode;
    uint8_t flags;
    bool poll_short_commands; /**< Poll page-load/READY commands on this device. */
} bsp_nand_flash_config_t;

#define BSP_NAND_FLASH_DEFAULT_CONFIG() {                     \
    .clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ,        \
    .queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE,          \
    .max_transfer_sz = BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ,\
    .gc_factor = 0,                                           \
    .io_mode = SPI_NAND_IO_MODE_QIO,                \
    .flags = SPI_DEVICE_HALFDUPLEX,                           \
    .poll_short_commands = true,                             \
}

esp_err_t bsp_nand_flash_init(const bsp_nand_flash_config_t *config,
                              spi_nand_flash_device_t **out_handle);
/** Initialize the exclusive block-device path (requires NAND_FLASH_ENABLE_BDL).
 * On success the caller owns the returned device. Its release operation frees
 * the NAND, SPI device and bus, including when a filesystem mount fails.
 * The legacy page path and block-device path cannot coexist on this chip.
 * Stop all IO before release; deinit rejects a live block-device owner. */
esp_err_t bsp_nand_flash_init_blockdev(const bsp_nand_flash_config_t *config,
                                    esp_blockdev_handle_t *out_handle);
/** Release legacy resources; release an owned block device through its ops. */
esp_err_t bsp_nand_flash_deinit(void);
spi_nand_flash_device_t *bsp_nand_flash_get_handle(void);

/** Sync NAND and leave the bus idle (CS# high standby). No-op if not initialized. */
esp_err_t bsp_nand_flash_enter_power_save(void);

/** Clear the power-save marker after `bsp_nand_flash_enter_power_save()`. */
esp_err_t bsp_nand_flash_exit_power_save(void);

#ifdef __cplusplus
}
#endif
