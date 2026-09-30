// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

/* PPA has four fractional scaling bits. Choose the largest even source
 * square that scales exactly to the destination, avoiding unpainted borders.
 * For SC101IOT's 720-line frame this is 640 at 3/4, not 720 at 2/3.
 * For OV3640's 768-line frame it is 768 at 5/8. */
#define PREVIEW_PPA_SCALE_STEPS 16U

static inline uint32_t preview_crop_size(uint32_t width, uint32_t height,
                                         uint32_t output_size)
{
    uint32_t crop = (width < height ? width : height) & ~1U;
    const uint32_t scaled_output = output_size * PREVIEW_PPA_SCALE_STEPS;
    for (; crop >= 2U; crop -= 2U) {
        if (scaled_output % crop == 0U && scaled_output / crop > 0U &&
            scaled_output / crop < 256U) {
            return crop;
        }
    }
    return 0;
}
