// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sky_hop_save_load(uint16_t *best_score);
esp_err_t sky_hop_save_best_score(uint16_t best_score);
esp_err_t sky_hop_save_flush(void);

#ifdef __cplusplus
}
#endif
