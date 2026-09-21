#pragma once
#include "lfs.h"
#include <stdint.h>

/* Read the CRC-validated superblock using LittleFS itself. Never writes media. */
int nand_lfs_probe(const struct lfs_config *geometry, uint32_t capacity,
                   uint32_t *stored_blocks);
