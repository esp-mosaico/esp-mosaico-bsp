/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_littlefs.h"
#include "nand_lfs_probe.h"
#include "nand_page_cache.h"
#include "esp_heap_caps.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_blockdev.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "spi_nand_flash.h"

static const char *TAG = "nand_littlefs";

typedef struct {
    esp_blockdev_t blockdev;
    spi_nand_flash_device_t *flash;
    uint32_t page_size;
    uint32_t page_count;
} nand_littlefs_context_t;

static nand_littlefs_context_t s_nand;
static bool s_mounted;
/* Access is serialized by LittleFS; pre-mount probe runs before VFS registration. */
static nand_page_cache_t s_read_cache;

static esp_err_t nand_blockdev_read(esp_blockdev_handle_t handle,
                                    uint8_t *destination,
                                    size_t destination_size,
                                    uint64_t source_address,
                                    size_t read_length)
{
    ESP_RETURN_ON_FALSE(handle && destination, ESP_ERR_INVALID_ARG, TAG,
                        "invalid NAND read arguments");
    nand_littlefs_context_t *context = handle->ctx;
    ESP_RETURN_ON_FALSE(context && context->flash, ESP_ERR_INVALID_STATE, TAG,
                        "NAND is not initialized");
    ESP_RETURN_ON_FALSE(destination_size >= read_length, ESP_ERR_INVALID_SIZE, TAG,
                        "NAND destination buffer is too small");
    ESP_RETURN_ON_FALSE(source_address % context->page_size == 0 &&
                            read_length % context->page_size == 0,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "LittleFS read is not aligned to NAND logical page size %" PRIu32,
                        context->page_size);
    ESP_RETURN_ON_FALSE(source_address <= handle->geometry.disk_size &&
                            read_length <= handle->geometry.disk_size - source_address,
                        ESP_ERR_INVALID_SIZE, TAG, "NAND read is out of range");

    uint32_t page = (uint32_t)(source_address / context->page_size);
    const uint32_t count = (uint32_t)(read_length / context->page_size);
    for (uint32_t index = 0; index < count; ++index) {
        uint8_t *out=destination+((size_t)index*context->page_size);
        if(nand_page_cache_get(&s_read_cache,page+index,out))continue;
        ESP_RETURN_ON_ERROR(spi_nand_flash_read_page(
                                context->flash,
                                out,
                                page + index),
                            TAG, "read NAND logical page %" PRIu32 " failed", page + index);
        nand_page_cache_put(&s_read_cache,page+index,out);
    }
    return ESP_OK;
}

static esp_err_t nand_blockdev_sync(esp_blockdev_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle && handle->ctx, ESP_ERR_INVALID_ARG, TAG,
                        "invalid NAND sync arguments");
    nand_littlefs_context_t *context = handle->ctx;
    return spi_nand_flash_sync(context->flash);
}


/* Dhara exposes rewritable logical pages. Never erase physical NAND blocks:
 * other logical pages may belong to existing music or filesystem metadata. */
static esp_err_t nand_blockdev_write(esp_blockdev_handle_t handle, const uint8_t *data,
                                     uint64_t address, size_t length)
{
    ESP_RETURN_ON_FALSE(handle && handle->ctx && data, ESP_ERR_INVALID_ARG, TAG, "bad write");
    nand_littlefs_context_t *c = handle->ctx;
    ESP_RETURN_ON_FALSE(address % c->page_size == 0 && length % c->page_size == 0 &&
                        address <= handle->geometry.disk_size &&
                        length <= handle->geometry.disk_size - address,
                        ESP_ERR_INVALID_SIZE, TAG, "unaligned/out-of-range write");
    for (size_t offset = 0; offset < length; offset += c->page_size) {
        /* Invalidate before attempting a write, including failed/partial writes. */
        nand_page_cache_invalidate(&s_read_cache,(address+offset)/c->page_size);
        ESP_RETURN_ON_ERROR(spi_nand_flash_write_page(c->flash, data + offset,
                            (address + offset) / c->page_size), TAG, "NAND write failed");
    }
    return ESP_OK;
}

static esp_err_t nand_blockdev_erase(esp_blockdev_handle_t handle, uint64_t address, size_t length)
{
    ESP_RETURN_ON_FALSE(handle && handle->ctx, ESP_ERR_INVALID_ARG, TAG, "bad erase");
    nand_littlefs_context_t *c = handle->ctx;
    ESP_RETURN_ON_FALSE(address % c->page_size == 0 && length % c->page_size == 0 &&
                        address <= handle->geometry.disk_size &&
                        length <= handle->geometry.disk_size - address,
                        ESP_ERR_INVALID_SIZE, TAG, "unaligned/out-of-range erase");
    /* Logical overwrite-capable media: each program replaces a complete page.
     * LittleFS uses a full-page cache/program unit and commit CRCs; it does not
     * require erased bytes here. Writing FF would double Dhara write/GC traffic.
     * Physical erase remains exclusively owned by Dhara. */
    return ESP_OK;
}

static const esp_blockdev_ops_t s_nand_blockdev_ops = {
    .read = nand_blockdev_read,
    .write = nand_blockdev_write,
    .erase = nand_blockdev_erase,
    .sync = nand_blockdev_sync,
    .ioctl = NULL,
    .release = NULL,
};

static int probe_read(const struct lfs_config *config, lfs_block_t block,
                      lfs_off_t offset, void *data, lfs_size_t size)
{
    if (offset > config->block_size || size > config->block_size - offset) return LFS_ERR_IO;
    uint64_t address = (uint64_t)block * config->block_size + offset;
    /* This callback always checks against the actual device capacity. */
    return nand_blockdev_read(config->context, data, size, address, size) == ESP_OK
           ? 0 : LFS_ERR_IO;
}

esp_err_t nand_littlefs_mount(const char *base_path)
{
    ESP_RETURN_ON_FALSE(base_path && base_path[0] == '/', ESP_ERR_INVALID_ARG, TAG,
                        "LittleFS base path must be absolute");
    ESP_RETURN_ON_FALSE(!s_mounted && !s_nand.flash, ESP_ERR_INVALID_STATE, TAG,
                        "NAND LittleFS is already mounted");

    int64_t started=esp_timer_get_time();
    esp_err_t ret = bsp_nand_flash_init(NULL, &s_nand.flash);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "initialize BSP NAND flash failed");
    int64_t initialized=esp_timer_get_time();
    ret = spi_nand_flash_get_page_count(s_nand.flash, &s_nand.page_count);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "get NAND logical page count failed");
    ret = spi_nand_flash_get_page_size(s_nand.flash, &s_nand.page_size);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "get NAND logical page size failed");
    ESP_GOTO_ON_FALSE(s_nand.page_count > 0 && s_nand.page_size >= 128,
                      ESP_ERR_INVALID_SIZE, fail, TAG, "invalid NAND geometry");
    s_read_cache.page_size=s_nand.page_size;
    s_read_cache.data=heap_caps_malloc((size_t)NAND_CACHE_SLOTS*s_nand.page_size,
                                       MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,"NAND logical read cache: %u bytes",s_read_cache.data?
             (unsigned)(NAND_CACHE_SLOTS*s_nand.page_size):0);

    s_nand.blockdev = (esp_blockdev_t) {
        .ctx = &s_nand,
        .device_flags = {
            .read_only = 0,
            .encrypted = 0,
            .erase_before_write = 0,
            .and_type_write = 0,
            .default_val_after_erase = 0,
        },
        .geometry = {
            .disk_size = (uint64_t)s_nand.page_count * s_nand.page_size,
            .read_size = s_nand.page_size,
            /* Preserve the existing LittleFS logical-page geometry. */
            .write_size = s_nand.page_size,
            .erase_size = s_nand.page_size,
            .recommended_write_size = s_nand.page_size,
            .recommended_read_size = s_nand.page_size,
            .recommended_erase_size = s_nand.page_size,
        },
        .ops = &s_nand_blockdev_ops,
    };

    const struct lfs_config probe_config = {
        .context = &s_nand.blockdev,
        .read = probe_read,
        .read_size = s_nand.page_size,
        .prog_size = s_nand.page_size,
        .block_size = s_nand.page_size,
        .block_cycles = -1,
        .cache_size = s_nand.page_size,
        .lookahead_size = 32,
    };
    uint32_t stored_blocks = 0;
    int probe_result = nand_lfs_probe(&probe_config, s_nand.page_count, &stored_blocks);
    ESP_GOTO_ON_FALSE(probe_result == 0, ESP_FAIL, fail, TAG,
                      "read-only LittleFS geometry probe failed (%d), capacity=%" PRIu32
                      "; NAND was not formatted", probe_result, s_nand.page_count);
    ESP_LOGI(TAG, "LittleFS geometry: stored=%" PRIu32 ", device=%" PRIu32
             " pages; preserving existing filesystem size", stored_blocks, s_nand.page_count);
    s_nand.blockdev.geometry.disk_size = (uint64_t)stored_blocks * s_nand.page_size;
    int64_t probed=esp_timer_get_time();

    const esp_vfs_littlefs_conf_t config = {
        .base_path = base_path,
        .partition_label = NULL,
        .partition = NULL,
        .blockdev = &s_nand.blockdev,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    ret = esp_vfs_littlefs_register(&config);
    ESP_GOTO_ON_ERROR(ret, fail, TAG,
                      "mount existing NAND LittleFS at %s failed; NAND was not formatted",
                      base_path);

    s_mounted = true;
    /* Requesting used_bytes invokes lfs_fs_size(), walking every file's blocks.
     * It is diagnostic-only and must not delay startup on a large NAND volume. */
    ESP_LOGI(TAG,"LittleFS mounted read-write at %s: total=%" PRIu64
             " bytes; init=%lld ms, probe=%lld ms, mount=%lld ms (no usage scan)",
             base_path,s_nand.blockdev.geometry.disk_size,
             (long long)((initialized-started)/1000),(long long)((probed-initialized)/1000),
             (long long)((esp_timer_get_time()-probed)/1000));
    return ESP_OK;

fail:
    heap_caps_free(s_read_cache.data);
    s_read_cache=(nand_page_cache_t){0};
    if (s_nand.flash) {
        (void)bsp_nand_flash_deinit();
    }
    s_nand = (nand_littlefs_context_t) {0};
    return ret;
}
