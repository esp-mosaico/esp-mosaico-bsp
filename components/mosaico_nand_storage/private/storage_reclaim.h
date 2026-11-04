// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_blockdev.h"

struct lfs;
typedef struct {
    uint32_t live_pages, examined_pages, discarded_pages;
} storage_reclaim_stats_t;
esp_err_t storage_reclaim_run(struct lfs *fs, esp_blockdev_handle_t dev, void *stats);
