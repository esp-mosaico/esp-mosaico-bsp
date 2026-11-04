// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_blockdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Local Mosaico fork interface, available for ESP-IDF 6+ block devices. */
struct lfs;
typedef esp_err_t (*mosaico_littlefs_fn)(struct lfs *, esp_blockdev_handle_t, void *);
/* Read-only, cancelable committed usage; caller excludes unmount/format. */
esp_err_t mosaico_littlefs_usage(esp_blockdev_handle_t dev, size_t *total, size_t *used,
                                bool (*cancel)(void *), void *ctx);
/* The caller owns its mount lifecycle/reservation and excludes unmount/format.
 * Runs synchronously under this filesystem's VFS lock. The callback must not
 * recurse into VFS operations or retain the borrowed filesystem pointer.
 * Reclamation callbacks must additionally reject ANY open file/directory. */
esp_err_t mosaico_littlefs_with_fs(esp_blockdev_handle_t dev, mosaico_littlefs_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
