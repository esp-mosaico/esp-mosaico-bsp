#include <assert.h>
#include <stdio.h>
#include "voltage_conversion.h"

int main(void)
{
    int mv;
    assert(!light_raw_to_mv(0, 0, 0, &mv));
    assert(!light_raw_to_mv(0, 100, -1, &mv));
    assert(!light_raw_to_mv(0, 131072, 50, &mv));
    assert(!light_raw_to_mv(-1, 0, 1000, &mv));
    assert(light_raw_to_mv(100, 100, 100100, &mv) && mv == 0);
    assert(light_raw_to_mv(50100, 100, 100100, &mv) && mv == 1000);
    assert(light_raw_to_mv(100100, 100, 100100, &mv) && mv == 2000);
    assert(light_raw_to_mv(0, 100, 100100, &mv) && mv == -2);
    assert(light_raw_to_mv(110100, 100, 100100, &mv) && mv == 2200);
    assert(light_raw_to_mv(131071, 0, 1, &mv) && mv == 262142000);
    assert(light_raw_to_mv(2197, 2197, 162, &mv) && mv == 0);
    assert(light_raw_to_mv(162, 2197, 162, &mv) && mv == 2000);
    assert(light_raw_to_mv(1180, 2197, 162, &mv) && mv == 999);
    assert(light_raw_to_mv(1179, 2197, 162, &mv) && mv == 1000);
    assert(light_raw_to_mv(0, 2197, 162, &mv) && mv > 2000);
    assert(light_raw_to_mv(2300, 2197, 162, &mv) && mv < 0);
    puts("voltage conversion: PASS");
}
