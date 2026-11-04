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

/* The display must not turn the ~20 ms acquisition cadence into ~270 ms gaps.
 * Exercise every phase of a 50 Hz stream, both directions and pulse polarities. */
static void test_fast_acquisition(void)
{
    unsigned detected = 0;
    for (unsigned direction = 0; direction < 2; ++direction) {
        for (unsigned polarity = 0; polarity < 2; ++polarity) {
            for (unsigned phase = 0; phase < 20; ++phase) {
                tof_gesture_init(&s, 60, 1500);
                unsigned events = 0;
                for (unsigned cycle = 0; cycle < 80; ++cycle) {
                    unsigned t = cycle * 20;
                    unsigned lt = t + (cycle % 2 ? 13 : 3);
                    unsigned rt = t + (cycle % 2 ? 3 : 13);
                    unsigned l = triangle(lt, 400 + phase + (direction ? 70 : 0));
                    unsigned r = triangle(rt, 400 + phase + (direction ? 0 : 70));
                    if (polarity) { l = 2000 - l; r = 2000 - r; }
                    tof_gesture_event_t event = tof_gesture_update_timed(&s, t + 19,
                        true, l, lt, true, r, rt);
                    if (event != TOF_GESTURE_NONE) {
                        assert(event == (direction ? TOF_GESTURE_LEFT : TOF_GESTURE_RIGHT));
                        ++events;
                    }
                }
                assert(events == 1);
                detected += events;
            }
        }
    }
    printf("50 Hz phase/polarity sweep: %u/80\n", detected);
}

static void test_brief_dropouts(void)
{
    for (unsigned direction = 0; direction < 2; ++direction) {
        warmup(direction ? UINT32_MAX - 500 : 0);
        now += 40;
        assert(tof_gesture_update(&s, now, true, direction ? 1000 : 1100,
                                 true, direction ? 1100 : 1000) == TOF_GESTURE_NONE);
        /* Three missing pairs at 50 Hz must not erase a genuine excursion. */
        for (unsigned i = 0; i < 3; ++i) {
            now += 20;
            assert(tof_gesture_update(&s, now, false, 0, true, 1000) == TOF_GESTURE_NONE);
            assert(s.ready);
        }
        now += 20;
        assert(tof_gesture_update(&s, now, true, direction ? 1100 : 1000,
                                 true, direction ? 1000 : 1100) == TOF_GESTURE_NONE);
        now += 40;
        assert(tof_gesture_update(&s, now, true, 1000, true, 1000) ==
               (direction ? TOF_GESTURE_LEFT : TOF_GESTURE_RIGHT));
    }
    warmup(0);
    now += 40;
    assert(tof_gesture_update(&s, now, true, 1100, true, 1000) == TOF_GESTURE_NONE);
    now += 20;
    assert(tof_gesture_update(&s, now, false, 0, true, 1100) == TOF_GESTURE_NONE);
    now += 100;
    assert(tof_gesture_update(&s, now, true, 1000, true, 1100) == TOF_GESTURE_NONE);
    assert(!s.ready); /* A blind interval over the grace limit cannot form a gesture. */
    assert(frame(1000, 1000) == TOF_GESTURE_NONE);
    warmup(0);
    now += 20;
    assert(tof_gesture_update_timed(&s, now, false, 1100, now - 501,
                                  true, 1100, now) == TOF_GESTURE_NONE);
    assert(!s.ready); /* Stale data bypasses the short-invalid-sample grace. */
}

static void test_empty_field(void)
{
    assert(tof_hand_classify(false, 0, 2, 300) == TOF_HAND_CLEAR);
    assert(tof_hand_classify(false, 65535, 2, 300) == TOF_HAND_CLEAR);
    assert(tof_hand_classify(false, 100, 1, 300) == TOF_HAND_UNKNOWN);
    assert(tof_hand_classify(false, 100, 5, 300) == TOF_HAND_UNKNOWN);
    assert(tof_hand_classify(true, 300, 0, 300) == TOF_HAND_NEAR);
    assert(tof_hand_classify(true, 330, 0, 300) == TOF_HAND_HOLD);
    assert(tof_hand_classify(true, 360, 0, 300) == TOF_HAND_CLEAR);
    unsigned detected = 0;
    for (unsigned direction = 0; direction < 2; ++direction) {
        for (unsigned phase = 0; phase < 20; ++phase) {
            tof_hand_gesture_t hand;
            tof_hand_gesture_init(&hand, 1500);
            unsigned events = 0;
            uint32_t origin = direction ? UINT32_MAX - 500 : 0;
            for (unsigned cycle = 0; cycle < 65; ++cycle) {
                unsigned t = cycle * 20;
                unsigned lt = t + (cycle % 2 ? 13 : 3);
                unsigned rt = t + (cycle % 2 ? 3 : 13);
                unsigned ls = 400 + phase + (direction ? 80 : 0);
                unsigned rs = 400 + phase + (direction ? 0 : 80);
                bool ln = lt >= ls && lt < ls + 140;
                bool rn = rt >= rs && rt < rs + 140;
                /* Both displays show -- outside the hand pulses: no background. */
                tof_hand_input_t l = tof_hand_classify(ln, ln ? 180 : 65535, ln ? 0 : 2, 300);
                tof_hand_input_t r = tof_hand_classify(rn, rn ? 220 : 0, rn ? 0 : 2, 300);
                tof_gesture_event_t event = tof_hand_gesture_update(&hand, origin + t + 19,
                    l, origin + lt, r, origin + rt);
                if (cycle == 10) assert(hand.ready);
                if (event != TOF_GESTURE_NONE) {
                    assert(event == (direction ? TOF_GESTURE_LEFT : TOF_GESTURE_RIGHT));
                    ++events;
                }
            }
            assert(events == 1);
            detected += events;
        }
    }
    tof_hand_gesture_t hand;
    tof_hand_gesture_init(&hand, 1500);
    for (unsigned t = 0; t < 300; t += 20) {
        assert(tof_hand_gesture_update(&hand, t, TOF_HAND_UNKNOWN, t, TOF_HAND_CLEAR, t) == TOF_GESTURE_NONE);
    }
    assert(!hand.ready); /* Hardware/sigma errors cannot arm an empty field. */
    for (unsigned t = 300; t < 500; t += 20) {
        assert(tof_hand_gesture_update(&hand, t, TOF_HAND_CLEAR, t, TOF_HAND_CLEAR, t) == TOF_GESTURE_NONE);
    }
    assert(hand.ready);
    for (unsigned t = 500; t < 800; t += 20) {
        tof_hand_input_t input = t < 620 ? TOF_HAND_NEAR : TOF_HAND_CLEAR;
        assert(tof_hand_gesture_update(&hand, t, input, t, input, t) == TOF_GESTURE_NONE);
    }
    assert(tof_hand_gesture_update(&hand, 800, TOF_HAND_NEAR, 800, TOF_HAND_CLEAR, 800) == TOF_GESTURE_NONE);
    assert(tof_hand_gesture_update(&hand, 820, TOF_HAND_CLEAR, 820, TOF_HAND_NEAR, 820) == TOF_GESTURE_NONE);
    assert(tof_hand_gesture_update(&hand, 840, TOF_HAND_CLEAR, 840, TOF_HAND_CLEAR, 840) == TOF_GESTURE_NONE);
    assert(!hand.ready); /* One-sample spikes cannot count as hand pulses. */
    printf("Empty-background phase sweep: %u/40; error, simultaneous and spike rejection passed\n", detected);
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
    test_fast_acquisition();
    test_brief_dropouts();
    test_empty_field();
    puts("Gesture tests passed");
    return 0;
}
