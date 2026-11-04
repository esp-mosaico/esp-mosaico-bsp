// SPDX-License-Identifier: Apache-2.0
#include "storage_volume.h"
#include "esp_log.h"
#include <limits.h>

/* This probe is unmounted before the VFS takes ownership. Its callbacks cannot
 * write, erase or repair media, including after a damaged superblock. */
#define ESP_LITTLEFS_RAW_API
#include "lfs.h"

static const char *TAG = "mosaico_nand_storage";

static int probe_read(const struct lfs_config *config, lfs_block_t block,
                      lfs_off_t offset, void *buffer, lfs_size_t size)
{
    esp_blockdev_handle_t source = config->context;
    const uint64_t address = (uint64_t)block * config->block_size + offset;
    if (offset > config->block_size || size > config->block_size - offset ||
        address > source->geometry.disk_size || size > source->geometry.disk_size - address)
        return LFS_ERR_IO;
    return source->ops->read(source, buffer, size, address, size) == ESP_OK ? 0 : LFS_ERR_IO;
}
static int probe_write(const struct lfs_config *config, lfs_block_t block,
                       lfs_off_t offset, const void *buffer, lfs_size_t size)
{
    (void)config; (void)block; (void)offset; (void)buffer; (void)size;
    return LFS_ERR_IO;
}
static int probe_erase(const struct lfs_config *config, lfs_block_t block)
{
    (void)config; (void)block;
    return LFS_ERR_IO;
}
static int probe_sync(const struct lfs_config *config)
{
    (void)config;
    return 0;
}
esp_err_t storage_volume_size(esp_blockdev_handle_t source, uint64_t *bytes)
{
    if (!source || !bytes || !source->ops || !source->ops->read)
        return ESP_ERR_INVALID_ARG;
    *bytes = 0;
    const size_t page = source->geometry.read_size;
    if (page < 128 || page > UINT32_MAX || (page & (page - 1)) ||
        source->geometry.write_size != page || source->geometry.erase_size != page ||
        !source->geometry.disk_size || source->geometry.disk_size % page)
        return ESP_ERR_INVALID_ARG;
    const struct lfs_config config = {
        .context = source, .read = probe_read, .prog = probe_write,
        .erase = probe_erase, .sync = probe_sync,
        .read_size = page, .prog_size = page, .block_size = page,
        .block_count = 0, /* Read the original size from the superblock. */
        .cache_size = page, .lookahead_size = 16, .block_cycles = -1,
    };
    lfs_t fs;
    int result = lfs_mount(&fs, &config);
    if (result)
        return result == LFS_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    struct lfs_fsinfo info;
    result = lfs_fs_stat(&fs, &info);
    const int unmount = lfs_unmount(&fs);
    if (result || unmount) return ESP_FAIL;
    const uint64_t stored = (uint64_t)info.block_count * info.block_size;
    if (!info.block_count || info.block_size != page || stored > source->geometry.disk_size) {
        ESP_LOGE(TAG, "NAND volume exceeds available geometry: stored=%llu available=%llu",
                 (unsigned long long)stored, (unsigned long long)source->geometry.disk_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (stored != source->geometry.disk_size)
        ESP_LOGW(TAG, "Keeping NAND volume geometry: stored=%llu available=%llu",
                 (unsigned long long)stored, (unsigned long long)source->geometry.disk_size);
    *bytes = stored;
    return ESP_OK;
}
