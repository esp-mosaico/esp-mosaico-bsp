/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mosaico_nand_storage.h"
#include "storage_blockdev.h"
#include "storage_volume.h"
#include "storage_reclaim.h"
#include "mosaico_nand_extension.h"
#include "mosaico_littlefs_extension.h"
#include "esp_littlefs.h"
#include "esp_check.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <string.h>
#define ESP_LITTLEFS_RAW_API
#include "lfs.h"

typedef struct {
    esp_blockdev_handle_t blockdev;
    bool mounted;
    bool usage_valid;
    uint32_t usage_generation;
    uint64_t usage_total, usage_used;
} nand_storage_context_t;
static nand_storage_context_t s_nand_storage;
static atomic_flag s_operation = ATOMIC_FLAG_INIT;
static atomic_int s_nand_status = ESP_ERR_INVALID_STATE;
static atomic_int s_recoverability = MOSAICO_NAND_STORAGE_FORMAT_NOT_NEEDED;
static const char *TAG = "mosaico_nand_storage";

static bool begin_operation(void)
{
    return !atomic_flag_test_and_set(&s_operation);
}
static void end_operation(esp_err_t err)
{
    atomic_store(&s_nand_status, err == ESP_ERR_INVALID_STATE ? ESP_FAIL : err);
    atomic_flag_clear(&s_operation);
}
static void cleanup_partial(nand_storage_context_t *ctx)
{
    if (ctx->blockdev && !ctx->mounted) {
        /* The wrapper clears ctx->blockdev as part of release. */
        (void)ctx->blockdev->ops->release(ctx->blockdev);
    }
}

static esp_err_t ensure_mounted(nand_storage_context_t *ctx, bool format, const mosaico_nand_storage_config_t *config)
{
    if (ctx->mounted) {
        atomic_store(&s_recoverability, MOSAICO_NAND_STORAGE_FORMAT_NOT_NEEDED);
        return ESP_OK;
    }
    /* Until the filesystem has been read successfully, control, SPI and chip
     * initialization failures are not evidence that formatting can help. */
    atomic_store(&s_recoverability, MOSAICO_NAND_STORAGE_HARDWARE_OR_UNKNOWN);
    ctx->usage_valid = false;

    esp_blockdev_handle_t source = NULL;
    esp_err_t err = bsp_nand_flash_init_blockdev(&config->flash, &source);
    if (err != ESP_OK) return err;
    storage_blockdev_clear_error();
    err = storage_blockdev_wrap(source, config->read_cache_bytes, &ctx->blockdev);
    if (err != ESP_OK) {
        (void)source->ops->release(source);
        return err;
    }
    uint64_t volume_bytes = ctx->blockdev->geometry.disk_size;
    if (!format) {
        /* Preserve existing geometry during normal mounts. Explicit recovery
         * must also work when the old superblock cannot be read. */
        err = storage_volume_size(ctx->blockdev, &volume_bytes);
        if (err != ESP_OK) {
            atomic_store(&s_recoverability,
                         storage_blockdev_read_error() != ESP_OK
                             ? MOSAICO_NAND_STORAGE_RETRYABLE_IO
                             : err == ESP_ERR_NO_MEM
                                   ? MOSAICO_NAND_STORAGE_HARDWARE_OR_UNKNOWN
                                   : MOSAICO_NAND_STORAGE_RECOVERABLE_FORMAT);
            cleanup_partial(ctx);
            return err;
        }
    }
    /* Dhara capacity can change as its bad-block estimate is restored. Keep
     * existing geometry unless the user requested a new filesystem. */
    ctx->blockdev->geometry.disk_size = volume_bytes;

    if (format) {
        uint32_t confirm = MOSAICO_NAND_DISCARD_CONFIRM;
        err = ctx->blockdev->ops->ioctl(ctx->blockdev, MOSAICO_NAND_DISCARD_ALL, &confirm);
        if (err == ESP_OK) err = esp_littlefs_format_blockdev(ctx->blockdev);
        if (err != ESP_OK) {
            atomic_store(&s_recoverability,
                         storage_blockdev_read_error() != ESP_OK
                             ? MOSAICO_NAND_STORAGE_RETRYABLE_IO
                             : MOSAICO_NAND_STORAGE_HARDWARE_OR_UNKNOWN);
            cleanup_partial(ctx);
            return err;
        }
    }

    const esp_vfs_littlefs_conf_t mount_config = {
        .base_path = config->base_path,
        .blockdev = ctx->blockdev,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
        .lookahead_size = 8192,
    };
    err = esp_vfs_littlefs_register(&mount_config);
    if (err != ESP_OK) {
        atomic_store(&s_recoverability,
                     storage_blockdev_read_error() != ESP_OK
                         ? MOSAICO_NAND_STORAGE_RETRYABLE_IO
                         : err == ESP_FAIL
                               ? MOSAICO_NAND_STORAGE_RECOVERABLE_FORMAT
                               : MOSAICO_NAND_STORAGE_HARDWARE_OR_UNKNOWN);
        /* The wrapper clears its owner slot when VFS releases it. Early
         * validation/allocation failures may leave ownership with us. */
        cleanup_partial(ctx);
        return err;
    }
    ctx->mounted = true;
    atomic_store(&s_recoverability, MOSAICO_NAND_STORAGE_FORMAT_NOT_NEEDED);

    ESP_LOGI(TAG, "NAND LittleFS mounted read-write");
    return ESP_OK;
}

static bool valid_config(const mosaico_nand_storage_config_t *config)
{
    return config->base_path && config->base_path[0] == '/';
}

esp_err_t mosaico_nand_storage_mount(const mosaico_nand_storage_config_t *config)
{
    mosaico_nand_storage_config_t active = config ? *config :
        (mosaico_nand_storage_config_t)MOSAICO_NAND_STORAGE_DEFAULT_CONFIG();
    if (!valid_config(&active)) return ESP_ERR_INVALID_ARG;
    if (!begin_operation()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ensure_mounted(&s_nand_storage, false, &active);
    end_operation(err);
    return err;
}

static esp_err_t require_closed(struct lfs *fs, esp_blockdev_handle_t dev, void *arg)
{
    (void)dev; (void)arg;
    return fs->mlist ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t unmount(nand_storage_context_t *ctx)
{
    if (!ctx->mounted) return ESP_OK;
    esp_err_t err = mosaico_littlefs_with_fs(ctx->blockdev, require_closed, NULL);
    if (err != ESP_OK) return err;
    err = esp_vfs_littlefs_unregister_blockdev(ctx->blockdev);
    if (err == ESP_OK) {
        ctx->mounted = false;
        ctx->usage_valid = false;
    }
    return err;
}

esp_err_t mosaico_nand_storage_unmount(void)
{
    if (!begin_operation()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = unmount(&s_nand_storage);
    /* A rejected unmount must not turn a working volume into a media error. */
    if (err == ESP_OK) atomic_store(&s_nand_status, ESP_ERR_INVALID_STATE);
    atomic_flag_clear(&s_operation);
    return err;
}

esp_err_t mosaico_nand_storage_format(const mosaico_nand_storage_config_t *config)
{
    mosaico_nand_storage_config_t active = config ? *config :
        (mosaico_nand_storage_config_t)MOSAICO_NAND_STORAGE_DEFAULT_CONFIG();
    if (!valid_config(&active)) return ESP_ERR_INVALID_ARG;
    if (!begin_operation()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = unmount(&s_nand_storage);
    if (err != ESP_OK) {
        atomic_flag_clear(&s_operation);
        return err;
    }
    err = ensure_mounted(&s_nand_storage, true, &active);
    if (err == ESP_OK) storage_blockdev_clear_error();
    end_operation(err);
    return err;
}

esp_err_t mosaico_nand_storage_error(void)
{
    esp_err_t status = atomic_load(&s_nand_status);
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) return status;
    return storage_blockdev_read_error();
}

void mosaico_nand_storage_clear_error(void)
{
    if (atomic_load(&s_nand_status) == ESP_OK) storage_blockdev_clear_error();
}

mosaico_nand_storage_recoverability_t mosaico_nand_storage_recoverability(void)
{
    if (storage_blockdev_read_error() != ESP_OK) return MOSAICO_NAND_STORAGE_RETRYABLE_IO;
    return (mosaico_nand_storage_recoverability_t)atomic_load(&s_recoverability);
}

esp_err_t mosaico_nand_storage_info(uint64_t *total, uint64_t *used,
                                  bool (*cancel)(void *), void *arg)
{
    if (!total || !used) return ESP_ERR_INVALID_ARG;
    if (!begin_operation()) return ESP_ERR_INVALID_STATE;
    nand_storage_context_t *ctx = &s_nand_storage;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!ctx->mounted || (cancel && cancel(arg))) goto done;
    uint32_t generation = storage_blockdev_generation();
    if (ctx->usage_valid && ctx->usage_generation == generation) {
        *total = ctx->usage_total;
        *used = ctx->usage_used;
        err = ESP_OK;
        goto done;
    }
    size_t t = 0, u = 0;
    err = mosaico_littlefs_usage(ctx->blockdev, &t, &u, cancel, arg);
    if (err == ESP_OK && cancel && cancel(arg)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        *total = t; *used = u;
        ctx->usage_total = t; ctx->usage_used = u;
        ctx->usage_generation = generation;
        ctx->usage_valid = generation == storage_blockdev_generation();
    }
done:
    atomic_flag_clear(&s_operation);
    return err;
}

esp_err_t mosaico_nand_storage_reclaim(void)
{
#if CONFIG_MOSAICO_NAND_STORAGE_RECLAIM
    if (!begin_operation()) return ESP_ERR_INVALID_STATE;
    nand_storage_context_t *ctx = &s_nand_storage;
    storage_reclaim_stats_t stats = {0};
    esp_err_t err = ctx->mounted ?
        mosaico_littlefs_with_fs(ctx->blockdev, storage_reclaim_run, &stats) : ESP_ERR_INVALID_STATE;
    ctx->usage_valid = false;
    atomic_flag_clear(&s_operation);
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
