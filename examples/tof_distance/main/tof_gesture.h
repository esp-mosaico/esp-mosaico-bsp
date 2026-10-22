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
    uint32_t last_valid_frame_ms;
    uint32_t cooldown_started_ms;
    bool have_frame;
    bool have_valid_frame;
    bool missing_samples;
    bool cooling;
    bool ready;
} tof_gesture_t;

void tof_gesture_init(tof_gesture_t *state, unsigned threshold_mm, uint32_t window_ms);
/* Feed a fresh left/right pair. Missing pairs are tolerated for up to 80 ms;
 * invalid distances are never consumed. Longer gaps cancel a partial gesture.
 * uint32_t millisecond timestamps may wrap. No hardware or LVGL dependencies. */
tof_gesture_event_t tof_gesture_update(tof_gesture_t *state, uint32_t now_ms,
                                     bool left_valid, unsigned left_mm,
                                     bool right_valid, unsigned right_mm);
tof_gesture_event_t tof_gesture_update_timed(tof_gesture_t *state, uint32_t now_ms,
    bool left_valid, unsigned left_mm, uint32_t left_ms,
    bool right_valid, unsigned right_mm, uint32_t right_ms);

typedef enum {
    TOF_HAND_UNKNOWN, /* Unusable reading, not evidence of an empty field. */
    TOF_HAND_CLEAR,   /* Far valid target or signal-fail/no confident return. */
    TOF_HAND_NEAR,
    TOF_HAND_HOLD,    /* Distance inside the near/far hysteresis band. */
} tof_hand_input_t;

typedef struct {
    bool active, done;
    unsigned near_samples;
    uint32_t started_ms, ended_ms;
} tof_hand_channel_t;

typedef struct {
    tof_hand_channel_t channel[2];
    uint32_t window_ms, clear_since_ms, last_good_ms;
    bool learning_clear, have_good, ready;
} tof_hand_gesture_t;

/* Common range status 2 is signal fail on both supported sensors. Its distance
 * is ignored: it only denotes no confident target for near-field detection. */
tof_hand_input_t tof_hand_classify(bool valid, unsigned distance_mm,
                                 unsigned range_status, unsigned near_mm);
void tof_hand_gesture_init(tof_hand_gesture_t *state, uint32_t window_ms);
/* A clear field arms after 80 ms, including when no valid background exists.
 * Each hand pulse needs two near samples and 25 ms duration. The caller applies
 * shared cooldown when combining this detector with baseline recognition. */
tof_gesture_event_t tof_hand_gesture_update(tof_hand_gesture_t *state, uint32_t now_ms,
    tof_hand_input_t left, uint32_t left_ms, tof_hand_input_t right, uint32_t right_ms);
