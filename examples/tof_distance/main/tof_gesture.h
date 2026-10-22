/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TOF_GESTURE_NONE,
    TOF_GESTURE_LEFT,
    TOF_GESTURE_RIGHT,
} tof_gesture_event_t;

typedef struct {
    int baseline;
    unsigned stable_samples;
    bool active;
    bool done;
    int polarity;
    uint32_t started_ms;
    uint32_t observed_start_ms;
    uint32_t ended_ms;
    int previous_mm;
    uint32_t previous_ms;
    bool have_previous;
} tof_gesture_channel_t;

typedef struct {
    tof_gesture_channel_t channel[2]; /* 0 = left, 1 = right */
    unsigned threshold_mm;
    uint32_t window_ms;
    uint32_t last_frame_ms;
    uint32_t cooldown_started_ms;
    bool have_frame;
    bool cooling;
    bool ready;
} tof_gesture_t;

void tof_gesture_init(tof_gesture_t *state, unsigned threshold_mm, uint32_t window_ms);
/* Feed a fresh left/right pair. Invalid samples cancel any partial gesture.
 * uint32_t millisecond timestamps may wrap. No hardware or LVGL dependencies. */
tof_gesture_event_t tof_gesture_update(tof_gesture_t *state, uint32_t now_ms,
                                     bool left_valid, unsigned left_mm,
                                     bool right_valid, unsigned right_mm);
tof_gesture_event_t tof_gesture_update_timed(tof_gesture_t *state, uint32_t now_ms,
    bool left_valid, unsigned left_mm, uint32_t left_ms,
    bool right_valid, unsigned right_mm, uint32_t right_ms);
