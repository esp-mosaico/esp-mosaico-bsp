/* SPDX-License-Identifier: Apache-2.0 */
#include "tof_gesture.h"
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME_GAP_MS 500U
#define MIN_PULSE_MS 25U
#define MIN_ORDER_GAP_MS 25U
#define COOLDOWN_MS 800U
#define STABLE_SAMPLES 3U

/* Estimate a threshold crossing between real sample timestamps. This avoids
 * assigning both sensors the end-of-frame timestamp and losing sub-frame order. */
static uint32_t crossing(const tof_gesture_channel_t *c, int value, int level, uint32_t now)
{
    if (!c->have_previous || value == c->previous_mm) return now;
    int64_t fraction = (int64_t)(level - c->previous_mm) * (uint32_t)(now - c->previous_ms);
    int64_t offset = fraction / (value - c->previous_mm);
    if (offset < 0 || offset > (uint32_t)(now - c->previous_ms)) return now;
    return c->previous_ms + (uint32_t)offset;
}

static void reset_channels(tof_gesture_t *s)
{
    memset(s->channel, 0, sizeof(s->channel));
    s->ready = false;
}

void tof_gesture_init(tof_gesture_t *s, unsigned threshold_mm, uint32_t window_ms)
{
    memset(s, 0, sizeof(*s));
    s->threshold_mm = threshold_mm < 2 ? 2 : threshold_mm;
    s->window_ms = window_ms;
}

tof_gesture_event_t tof_gesture_update(tof_gesture_t *s, uint32_t now,
                                     bool left_valid, unsigned left_mm,
                                     bool right_valid, unsigned right_mm)
{
    return tof_gesture_update_timed(s, now, left_valid, left_mm, now,
                                   right_valid, right_mm, now);
}

tof_gesture_event_t tof_gesture_update_timed(tof_gesture_t *s, uint32_t now,
    bool left_valid, unsigned left_mm, uint32_t left_ms,
    bool right_valid, unsigned right_mm, uint32_t right_ms)
{
    if (s->have_frame && now - s->last_frame_ms > MAX_FRAME_GAP_MS) {
        reset_channels(s);
    }
    s->have_frame = true;
    s->last_frame_ms = now;
    if (!left_valid || !right_valid || now - left_ms > MAX_FRAME_GAP_MS ||
        now - right_ms > MAX_FRAME_GAP_MS) {
        reset_channels(s);
        return TOF_GESTURE_NONE;
    }
    if (s->cooling) {
        if (now - s->cooldown_started_ms < COOLDOWN_MS) return TOF_GESTURE_NONE;
        s->cooling = false;
    }
    const int values[2] = {(int)left_mm, (int)right_mm};
    const uint32_t times[2] = {left_ms, right_ms};
    const int release_mm = (int)s->threshold_mm / 2;
    if (!s->ready) {
        for (int i = 0; i < 2; ++i) {
            tof_gesture_channel_t *c = &s->channel[i];
            if (!c->stable_samples || abs(values[i] - c->baseline) > release_mm) {
                c->baseline = values[i];
                c->stable_samples = 1;
            } else {
                c->baseline = (c->baseline + values[i]) / 2;
                if (c->stable_samples < STABLE_SAMPLES) ++c->stable_samples;
            }
            c->previous_mm = values[i];
            c->previous_ms = times[i];
            c->have_previous = true;
        }
        s->ready = s->channel[0].stable_samples >= STABLE_SAMPLES &&
                   s->channel[1].stable_samples >= STABLE_SAMPLES;
        return TOF_GESTURE_NONE;
    }
    for (int i = 0; i < 2; ++i) {
        tof_gesture_channel_t *c = &s->channel[i];
        if ((c->active || c->done) && now - c->started_ms > s->window_ms) {
            reset_channels(s);
            return TOF_GESTURE_NONE;
        }
    }
    const bool idle = !s->channel[0].active && !s->channel[0].done &&
                      !s->channel[1].active && !s->channel[1].done;
    for (int i = 0; i < 2; ++i) {
        tof_gesture_channel_t *c = &s->channel[i];
        int delta = values[i] - c->baseline;
        if (c->done) continue;
        if (!c->active) {
            if (abs(delta) >= (int)s->threshold_mm) {
                c->active = true;
                c->polarity = delta > 0 ? 1 : -1;
                c->observed_start_ms = times[i];
                c->started_ms = crossing(c, values[i],
                    c->baseline + c->polarity * (int)s->threshold_mm, times[i]);
            } else if (idle && abs(delta) <= release_mm) {
                c->baseline = (c->baseline * 3 + values[i]) / 4;
            }
        } else if (abs(delta) <= release_mm) {
            uint32_t ended = crossing(c, values[i],
                c->baseline + c->polarity * release_mm, times[i]);
            if (ended - c->started_ms < MIN_PULSE_MS ||
                times[i] - c->observed_start_ms < MIN_PULSE_MS) {
                reset_channels(s);
                return TOF_GESTURE_NONE;
            }
            c->active = false;
            c->done = true;
            c->ended_ms = ended;
        } else if (delta * c->polarity < -release_mm) {
            /* Crossing to the opposite excursion is not a clean return. */
            reset_channels(s);
            return TOF_GESTURE_NONE;
        }
        c->previous_mm = values[i];
        c->previous_ms = times[i];
        c->have_previous = true;
    }
    const tof_gesture_channel_t *l = &s->channel[0], *r = &s->channel[1];
    if (!l->done || !r->done) return TOF_GESTURE_NONE;
    tof_gesture_event_t event = TOF_GESTURE_NONE;
    int32_t onset_gap = (int32_t)(r->started_ms - l->started_ms);
    int32_t end_gap = (int32_t)(r->ended_ms - l->ended_ms);
    if (l->polarity == r->polarity) {
        if (onset_gap >= (int32_t)MIN_ORDER_GAP_MS && end_gap >= 0) {
            event = TOF_GESTURE_RIGHT;
        } else if (onset_gap <= -(int32_t)MIN_ORDER_GAP_MS && end_gap <= 0) {
            event = TOF_GESTURE_LEFT;
        }
    }
    reset_channels(s);
    s->cooling = true;
    s->cooldown_started_ms = now;
    return event;
}
