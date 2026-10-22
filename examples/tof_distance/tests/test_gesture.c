/* SPDX-License-Identifier: Apache-2.0 */
/* Host test: cc -std=c11 -Wall -Wextra -Werror -I main
 * tests/test_gesture.c main/tof_gesture.c -o /tmp/tof_gesture_test */
#include "tof_gesture.h"
#include <assert.h>
#include <stdio.h>

static tof_gesture_t s;
static uint32_t now;
static tof_gesture_event_t frame(unsigned l, unsigned r)
{
    now += 150;
    return tof_gesture_update(&s, now, true, l, true, r);
}
static void warmup(uint32_t start)
{
    tof_gesture_init(&s, 60, 1500);
    now = start;
    assert(frame(1000, 1000) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE);
    assert(s.ready);
}
static void right_wave(unsigned excursion)
{
    assert(frame(excursion, 1000) == TOF_GESTURE_NONE);
    assert(frame(1000, excursion) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_RIGHT);
}

static unsigned triangle(uint32_t time, unsigned start)
{
    if (time <= start || time >= start + 200) return 1000;
    unsigned elapsed = time - start;
    return 1000 + (elapsed <= 100 ? elapsed : 200 - elapsed) * 2;
}

static void test_acquisition_phase(void)
{
    unsigned counts[2] = {0};
    for (unsigned direction = 0; direction < 2; ++direction) {
        for (unsigned phase = 0; phase < 60; phase += 5) {
            tof_gesture_init(&s, 60, 1500);
            unsigned detected = 0;
            for (unsigned cycle = 0; cycle < 25; ++cycle) {
                unsigned t = cycle * 60;
                /* Sequential reads with alternating first sensor. */
                unsigned lt = t + (cycle % 2 ? 35 : 10);
                unsigned rt = t + (cycle % 2 ? 10 : 35);
                unsigned ls = 400 + phase + (direction ? 70 : 0);
                unsigned rs = 400 + phase + (direction ? 0 : 70);
                tof_gesture_event_t result = tof_gesture_update_timed(&s, t + 50,
                    true, triangle(lt, ls), lt, true, triangle(rt, rs), rt);
                if (result != TOF_GESTURE_NONE) {
                    assert(result == (direction ? TOF_GESTURE_LEFT : TOF_GESTURE_RIGHT));
                    ++detected;
                }
            }
            assert(detected == 1);
            counts[direction] += detected;
        }
    }
    assert(counts[0] == counts[1]);
    for (unsigned phase = 0; phase < 60; phase += 5) {
        tof_gesture_init(&s, 60, 1500);
        for (unsigned cycle = 0; cycle < 25; ++cycle) {
            unsigned t = cycle * 60;
            unsigned lt = t + (cycle % 2 ? 35 : 10);
            unsigned rt = t + (cycle % 2 ? 10 : 35);
            assert(tof_gesture_update_timed(&s, t + 50,
                true, triangle(lt, 400 + phase), lt,
                true, triangle(rt, 400 + phase), rt) == TOF_GESTURE_NONE);
        }
    }
    printf("Timed phase sweep: right=%u/12 left=%u/12\n", counts[0], counts[1]);
}

int main(void)
{
    warmup(0);
    right_wave(1100); /* Increase then decrease. */
    assert(!s.ready);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    assert(frame(1000, 1100) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE); /* Cooldown. */

    warmup(0);
    right_wave(900); /* A hand often makes measured distance smaller. */
    warmup(0);
    assert(frame(1000, 1100) == TOF_GESTURE_NONE);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_LEFT);

    warmup(0);
    assert(frame(1100, 1100) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE); /* Simultaneous. */
    warmup(0);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    assert(frame(1100, 1100) == TOF_GESTURE_NONE);
    assert(frame(1000, 1100) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_RIGHT); /* Overlapping pulses. */

    warmup(0);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    assert(frame(1100, 1100) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_RIGHT); /* Same completion time. */
    warmup(0);
    assert(frame(1000, 1100) == TOF_GESTURE_NONE);
    assert(frame(1100, 1100) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_LEFT);

    warmup(0);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    assert(frame(1100, 1100) == TOF_GESTURE_NONE);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE); /* Opposite completion order. */
    warmup(0);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    assert(frame(1000, 900) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE); /* Different polarities. */

    warmup(0);
    for (int i = 0; i < 30; ++i) {
        assert(frame(1000 + i % 5, 1000 - i % 5) == TOF_GESTURE_NONE);
    }
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    for (int i = 0; i < 15; ++i) {
        assert(frame(1000, 1000) == TOF_GESTURE_NONE); /* Lone pulse expires. */
    }
    warmup(0);
    for (int i = 0; i < 15; ++i) {
        assert(frame(1100, 1000) == TOF_GESTURE_NONE); /* Long hold. */
    }
    assert(frame(1000, 1100) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE);

    warmup(0);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE);
    now += 150;
    assert(tof_gesture_update(&s, now, false, 0, true, 1100) == TOF_GESTURE_NONE);
    assert(!s.ready);
    assert(frame(1000, 1000) == TOF_GESTURE_NONE);

    warmup(0);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    now += 1000;
    assert(frame(1000, 1100) == TOF_GESTURE_NONE); /* Stale pair resets. */
    assert(frame(1000, 1000) == TOF_GESTURE_NONE);

    warmup(0);
    assert(frame(1100, 1000) == TOF_GESTURE_NONE);
    now += 10;
    assert(tof_gesture_update(&s, now, true, 1000, true, 1000) == TOF_GESTURE_NONE);
    assert(!s.ready); /* Short spike rejected. */

    warmup(UINT32_MAX - 650);
    right_wave(1100); /* Wraparound during gesture. */
    for (int i = 0; i < 8; ++i) {
        assert(frame(1000, 1000) == TOF_GESTURE_NONE);
    }
    assert(s.ready);
    right_wave(1100); /* Rearm after cooldown and stable background. */

    tof_gesture_init(&s, 60, 1500);
    now = 0;
    for (int i = 0; i < 3; ++i) assert(frame(500, 1200) == TOF_GESTURE_NONE);
    assert(s.ready);
    assert(frame(560, 1200) == TOF_GESTURE_NONE);
    assert(frame(530, 1260) == TOF_GESTURE_NONE);
    assert(frame(500, 1230) == TOF_GESTURE_RIGHT); /* Independent baselines and boundaries. */

    /* Fast waves: 50 ms paired frames, versus the original 150 ms fixtures. */
    warmup(0);
    now += 50;
    assert(tof_gesture_update(&s, now, true, 1100, true, 1000) == TOF_GESTURE_NONE);
    now += 50;
    assert(tof_gesture_update(&s, now, true, 1000, true, 1100) == TOF_GESTURE_NONE);
    now += 50;
    assert(tof_gesture_update(&s, now, true, 1000, true, 1000) == TOF_GESTURE_RIGHT);
    warmup(0);
    now += 50;
    assert(tof_gesture_update(&s, now, true, 1000, true, 900) == TOF_GESTURE_NONE);
    now += 50;
    assert(tof_gesture_update(&s, now, true, 900, true, 1000) == TOF_GESTURE_NONE);
    now += 50;
    assert(tof_gesture_update(&s, now, true, 1000, true, 1000) == TOF_GESTURE_LEFT);
    warmup(0);
    now += 50;
    assert(tof_gesture_update(&s, now, true, 1100, true, 1100) == TOF_GESTURE_NONE);
    now += 50;
    assert(tof_gesture_update(&s, now, true, 1000, true, 1000) == TOF_GESTURE_NONE);
    test_acquisition_phase();
    puts("Gesture tests passed");
    return 0;
}
