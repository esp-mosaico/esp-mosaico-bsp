// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_blockdev.h"

/* On success, the wrapper owns source, including release after mount failure.
 * The output slot must remain valid until release and is cleared by release.
 * Allocation failure for the optional PSRAM cache falls back to uncached IO. */
esp_err_t storage_blockdev_wrap(esp_blockdev_handle_t source, size_t cache_bytes,
                                esp_blockdev_handle_t *out);

/* Sticky physical read failure, reset only after successful recovery. */
esp_err_t storage_blockdev_read_error(void);
void storage_blockdev_clear_error(void);
/* Changes before every attempted mutation, including external VFS writes. */
uint32_t storage_blockdev_generation(void);

/* Called only under the filesystem lock, after all handles are closed and its
 * root is durable. live is a complete bitset including metadata and orphans.
 * Completes one candidate sweep, cooperating with the scheduler during IO. */
esp_err_t storage_blockdev_reclaim(esp_blockdev_handle_t dev, const uint8_t *live,
                                   uint32_t pages, uint32_t *examined, uint32_t *discarded);
