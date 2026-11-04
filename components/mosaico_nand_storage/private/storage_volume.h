// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_blockdev.h"

/* Probe an unmounted logical NAND volume without writing. The returned size
 * is the persisted filesystem size, bounded by the current FTL capacity.
 * The caller retains ownership of source on every return path. */
esp_err_t storage_volume_size(esp_blockdev_handle_t source, uint64_t *bytes);
