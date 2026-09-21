#include "nand_lfs_probe.h"

static int reject_program(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, const void *data, lfs_size_t size)
{
    (void)c; (void)block; (void)off; (void)data; (void)size;
    return LFS_ERR_IO;
}
static int reject_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c; (void)block;
    return LFS_ERR_IO;
}
static int readonly_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

int nand_lfs_probe(const struct lfs_config *geometry, uint32_t capacity,
                   uint32_t *stored_blocks)
{
    if (!geometry || !geometry->read || !capacity || !stored_blocks) return LFS_ERR_INVAL;
    *stored_blocks = 0;
    struct lfs_config config = *geometry;
    /* Zero means use the on-disk block count, not the current Dhara capacity. */
    config.block_count = 0;
    config.prog = reject_program;
    config.erase = reject_erase;
    config.sync = readonly_sync;
    lfs_t fs = {0};
    int err = lfs_mount(&fs, &config);
    if (err) return err;
    struct lfs_fsinfo info = {0};
    err = lfs_fs_stat(&fs, &info);
    if (!err && (!info.block_count || info.block_count > capacity ||
                 info.block_size != config.block_size)) err = LFS_ERR_INVAL;
    int unmount_err = lfs_unmount(&fs);
    if (!err) err = unmount_err;
    if (!err) *stored_blocks = info.block_count;
    return err;
}
