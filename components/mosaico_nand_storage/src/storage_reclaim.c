// SPDX-License-Identifier: Apache-2.0
#include "storage_reclaim.h"
#include "storage_blockdev.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#define ESP_LITTLEFS_RAW_API
#include "lfs.h"

typedef struct {
    uint8_t *bits;
    lfs_size_t count;
    uint32_t used;
} live_pages_t;

static int mark_live(void *ctx, lfs_block_t block)
{
    live_pages_t *live = ctx;
    if (block >= live->count) return LFS_ERR_CORRUPT;
    const uint8_t bit = 1U << (block % 8);
    if (!(live->bits[block / 8] & bit)) {
        live->bits[block / 8] |= bit;
        ++live->used;
    }
    return 0;
}

esp_err_t storage_reclaim_run(struct lfs *fs, esp_blockdev_handle_t dev, void *arg)
{
    storage_reclaim_stats_t *stats = arg;
    if (!fs || !dev || !stats || !fs->cfg) return ESP_ERR_INVALID_ARG;
    memset(stats, 0, sizeof(*stats));
    /* A read-only unlinked/open file need not be included in fs_traverse.
     * Refuse reclamation while ANY file/directory is open, rather than guessing
     * its liveness from a directory walk. The VFS lock prevents new opens. */
    if (fs->mlist) return ESP_ERR_INVALID_STATE;
    const lfs_size_t blocks = fs->block_count;
    if (!blocks || fs->cfg->block_size != dev->geometry.read_size ||
        (uint64_t)blocks * fs->cfg->block_size != dev->geometry.disk_size)
        return ESP_ERR_INVALID_SIZE;
    const size_t bytes = ((size_t)blocks + 7) / 8;
    uint8_t *bits = heap_caps_calloc(bytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bits) return ESP_ERR_NO_MEM;
    live_pages_t live = {.bits = bits, .count = blocks};
    /* Finish outstanding orphan/move recovery and persist its root BEFORE
     * deciding any mapping is dead. Traversal includes remaining orphans. */
    int result = lfs_fs_mkconsistent(fs);
    if (!result) result = fs->cfg->sync(fs->cfg);
    if (!result) result = lfs_fs_traverse(fs, mark_live, &live);
    esp_err_t err = result ? ESP_FAIL : storage_blockdev_reclaim(dev, bits, blocks,
        &stats->examined_pages, &stats->discarded_pages);
    stats->live_pages = live.used;
    free(bits);
    return err;
}
