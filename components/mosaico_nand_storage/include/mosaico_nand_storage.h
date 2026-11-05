/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "bsp/nand_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *base_path;
    bsp_nand_flash_config_t flash;
    size_t read_cache_bytes;
} mosaico_nand_storage_config_t;

#if CONFIG_MOSAICO_NAND_STORAGE_ENABLE
#if CONFIG_MOSAICO_NAND_STORAGE_QIO
#define MOSAICO_NAND_STORAGE_DEFAULT_IO_MODE SPI_NAND_IO_MODE_QIO
#else
#define MOSAICO_NAND_STORAGE_DEFAULT_IO_MODE SPI_NAND_IO_MODE_SIO
#endif
#if CONFIG_MOSAICO_NAND_STORAGE_POLL_SHORT
#define MOSAICO_NAND_STORAGE_DEFAULT_POLL_SHORT true
#else
#define MOSAICO_NAND_STORAGE_DEFAULT_POLL_SHORT false
#endif
#define MOSAICO_NAND_STORAGE_DEFAULT_CONFIG() { \
    .base_path = "/nand", \
    .flash = { \
        .clock_speed_hz = CONFIG_MOSAICO_NAND_STORAGE_CLOCK_MHZ * 1000000, \
        .queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE, \
        .max_transfer_sz = BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ, \
        .gc_factor = 0, \
        .io_mode = MOSAICO_NAND_STORAGE_DEFAULT_IO_MODE, \
        .flags = SPI_DEVICE_HALFDUPLEX, \
        .poll_short_commands = MOSAICO_NAND_STORAGE_DEFAULT_POLL_SHORT, \
    }, \
    .read_cache_bytes = CONFIG_MOSAICO_NAND_STORAGE_READ_CACHE_KIB * 1024U, \
}
#endif

typedef enum {
    MOSAICO_NAND_STORAGE_FORMAT_NOT_NEEDED,
    MOSAICO_NAND_STORAGE_RECOVERABLE_FORMAT,
    MOSAICO_NAND_STORAGE_RETRYABLE_IO,
    MOSAICO_NAND_STORAGE_HARDWARE_OR_UNKNOWN,
} mosaico_nand_storage_recoverability_t;

/* One volume on the board NAND. NULL selects the defaults. Mount preserves the
 * on-disk geometry and never formats automatically. Repeated mount is a no-op;
 * unmount before changing configuration. Failures release partial resources. */
esp_err_t mosaico_nand_storage_mount(const mosaico_nand_storage_config_t *config);
/* Stop and drain external VFS users before unmount/format. Open files or
 * directories reject the operation. Unmount is idempotent. */
esp_err_t mosaico_nand_storage_unmount(void);
/* Explicitly destructive: clear logical mappings and recreate/mount LittleFS.
 * The application must obtain user confirmation and quiesce its users first. */
esp_err_t mosaico_nand_storage_format(const mosaico_nand_storage_config_t *config);
esp_err_t mosaico_nand_storage_error(void);
/* Forget a latched read error of the mounted volume before a new job; a
 * persistent fault latches again on its next failing read. */
void mosaico_nand_storage_clear_error(void);
mosaico_nand_storage_recoverability_t mosaico_nand_storage_recoverability(void);
/* Read committed usage; cancellation preserves outputs. The callback must not
 * perform IO or call storage APIs. Mutations invalidate the usage cache. */
esp_err_t mosaico_nand_storage_info(uint64_t *total, uint64_t *used,
                                  bool (*cancel)(void *), void *ctx);
/* Explicit synchronous maintenance after commit. Rejects open handles; holds
 * the VFS lock through the complete traversal/TRIM and never formats data.
 * Concurrent lifecycle/maintenance calls return INVALID_STATE. */
esp_err_t mosaico_nand_storage_reclaim(void);

#ifdef __cplusplus
}
#endif
