/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Two-point board calibration, either slope; preserve out-of-range readings. */
static inline bool light_raw_to_mv(int raw, int zero, int two_volts, int *mv)
{
    if (!mv || raw < 0 || raw > 131071 || zero < 0 || zero > 131071 ||
            two_volts < 0 || two_volts > 131071 || two_volts == zero) {
        return false;
    }
    *mv = (int)(((int64_t)raw - zero) * 2000 / (two_volts - zero));
    return true;
}
