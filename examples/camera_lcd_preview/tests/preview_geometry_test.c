// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include "preview_geometry.h"

int main(void)
{
    assert(preview_crop_size(1280, 720, 480) == 640);
    assert(preview_crop_size(1024, 768, 480) == 768);
    assert(preview_crop_size(640, 480, 480) == 480);
    assert(preview_crop_size(320, 240, 480) == 240);
    assert(preview_crop_size(0, 720, 480) == 0);
    assert(preview_crop_size(1, 1, 480) == 0);
    for (unsigned height = 32; height <= 2048; ++height) {
        const unsigned crop = preview_crop_size(2048, height, 480);
        assert(crop > 0 && crop <= height && crop % 2 == 0);
        /* Match the hardware's integer arithmetic, not floating-point intent. */
        const unsigned scale = 480U * 16U / crop;
        assert(crop * scale / 16U == 480U);
    }
    return 0;
}
