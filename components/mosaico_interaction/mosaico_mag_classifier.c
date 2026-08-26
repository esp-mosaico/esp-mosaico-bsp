/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_mag_classifier.h"

#include <limits.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "mosaico_mag";

const mosaico_mag_calibration_t MOSAICO_MAG_CALIBRATION_S31_V1 = {
    .name = "esp-mosaico-v1",
    .filter_samples = 8,
    .saturation_limit = 30000,
    .left_reject_z = 650,
    .left_y_high = 1050,
    .down_y_both_sides = 60,
    .down_y_with_side = 300,
    .down_y_without_side = 300,
    .down_right_y_without_side = 240,
    .down_right_y_delta = 240,
    .up_right_y = -280,
    .up_right_y_delta = -300,
    .up_right_y_high = -400,
    .up_right_y_with_down = 20,
    .up_right_y_both_with_down = 80,
    .up_right_z_delta_with_down = 50,
    .up_right_y_fallback = -200,
    .up_left_z_with_down = 840,
    .baseline_right_y_min = -100,
    .baseline_right_y_max = 260,
    .baseline_right_y_max_span = 40,
};

static bool value_is_saturated(const mosaico_mag_calibration_t *calibration, int32_t value)
{
    return value <= -calibration->saturation_limit || value >= calibration->saturation_limit;
}

static mosaico_edge_mask_t classify_axes(
    const mosaico_mag_calibration_t *calibration,
    int32_t left_x,
    int32_t left_y,
    int32_t left_z,
    int32_t right_x,
    int32_t right_y,
    int32_t right_y_delta,
    int32_t right_z_delta)
{
    const bool has_left = value_is_saturated(calibration, left_x) &&
                          !(value_is_saturated(calibration, left_y) &&
                            left_z < calibration->left_reject_z);
    const bool has_right = value_is_saturated(calibration, right_x);
    const bool left_y_high = left_y > calibration->left_y_high;
    bool has_top = false;
    bool has_bottom = false;

    if (has_left && has_right) {
        if (left_y_high) {
            has_top = right_y_delta < calibration->up_right_y_delta;
        } else if (value_is_saturated(calibration, left_y)) {
            has_top = right_y < calibration->up_right_y ||
                      right_y_delta < calibration->up_right_y_delta;
        } else {
            has_bottom = left_y > calibration->down_y_both_sides;
            has_top = right_y < calibration->up_right_y ||
                      right_y_delta < calibration->up_right_y_delta ||
                      (has_bottom && (left_z >= calibration->up_left_z_with_down ||
                                      right_y < calibration->up_right_y_both_with_down));
            if (!has_bottom && right_y < calibration->up_right_y_fallback) {
                has_top = true;
            }
        }
    } else if (has_left) {
        if (!left_y_high) {
            /*
             * A standalone left neighbor drives left Y through the old bottom
             * band while it approaches. A real left+bottom capture also has a
             * positive right-Y signature, so require both axes before adding
             * bottom to an already detected left edge.
             */
            has_bottom = left_y > calibration->down_y_with_side &&
                         right_y > calibration->down_right_y_without_side;
            has_top = right_y < calibration->up_right_y ||
                      right_y_delta < calibration->up_right_y_delta ||
                      (has_bottom && right_y < calibration->up_right_y_with_down);
        }
    } else if (has_right) {
        if (left_y_high) {
            has_top = right_y < calibration->up_right_y_high ||
                      right_y_delta < calibration->up_right_y_delta;
        } else {
            has_bottom = left_y > calibration->down_y_with_side;
            has_top = right_y < calibration->up_right_y ||
                      right_y_delta < calibration->up_right_y_delta ||
                      (has_bottom && right_y < calibration->up_right_y_with_down);
        }
    } else {
        /* A vertical pair can cancel most of the right-Y response. Require the
         * measured right-Z rise before widening that narrow Y band, so a slow
         * bottom approach does not fabricate a top contact. */
        has_bottom = right_y_delta > calibration->down_right_y_delta ||
                     (!left_y_high && left_y > calibration->down_y_without_side);
        has_top = right_y_delta < calibration->up_right_y_delta ||
                  (has_bottom &&
                   (right_y < calibration->up_right_y_with_down ||
                    (right_y < calibration->up_right_y_both_with_down &&
                     right_z_delta > calibration->up_right_z_delta_with_down)));
    }

    mosaico_edge_mask_t mask = 0;
    if (has_top) {
        mask |= MOSAICO_EDGE_MASK_TOP;
    }
    if (has_right) {
        mask |= MOSAICO_EDGE_MASK_RIGHT;
    }
    if (has_bottom) {
        mask |= MOSAICO_EDGE_MASK_BOTTOM;
    }
    if (has_left) {
        mask |= MOSAICO_EDGE_MASK_LEFT;
    }
    return mask;
}

esp_err_t mosaico_mag_classifier_init(
    mosaico_mag_classifier_t *classifier,
    const mosaico_mag_calibration_t *calibration)
{
    ESP_RETURN_ON_FALSE(classifier && calibration, ESP_ERR_INVALID_ARG, TAG,
                        "classifier or calibration is null");
    ESP_RETURN_ON_FALSE(calibration->name && calibration->filter_samples > 0 &&
                        calibration->filter_samples <= MOSAICO_MAG_FILTER_MAX_SAMPLES &&
                        calibration->saturation_limit > 0 &&
                        calibration->down_right_y_delta > 0 &&
                        calibration->up_right_y_delta < 0 &&
                        calibration->up_right_z_delta_with_down > 0 &&
                        calibration->baseline_right_y_min < calibration->baseline_right_y_max &&
                        calibration->baseline_right_y_max_span > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid calibration profile");
    memset(classifier, 0, sizeof(*classifier));
    classifier->calibration = calibration;
    classifier->calibration_state = MOSAICO_MAG_CALIBRATING;
    ESP_LOGI(TAG, "classifier initialized: profile=%s filter_samples=%u",
             calibration->name, calibration->filter_samples);
    return ESP_OK;
}

void mosaico_mag_classifier_reset(mosaico_mag_classifier_t *classifier)
{
    if (!classifier) {
        return;
    }
    const mosaico_mag_calibration_t *calibration = classifier->calibration;
    memset(classifier, 0, sizeof(*classifier));
    classifier->calibration = calibration;
    classifier->calibration_state = MOSAICO_MAG_CALIBRATING;
}

static void clear_filter_history(mosaico_mag_classifier_t *classifier)
{
    memset(classifier->left_x, 0, sizeof(classifier->left_x));
    memset(classifier->left_y, 0, sizeof(classifier->left_y));
    memset(classifier->left_z, 0, sizeof(classifier->left_z));
    memset(classifier->right_x, 0, sizeof(classifier->right_x));
    memset(classifier->right_y, 0, sizeof(classifier->right_y));
    memset(classifier->right_z, 0, sizeof(classifier->right_z));
    classifier->write_index = 0;
    classifier->sample_count = 0;
}

esp_err_t mosaico_mag_classifier_process(
    mosaico_mag_classifier_t *classifier,
    const mosaico_mag_sample_t *sample,
    mosaico_mag_classification_t *classification)
{
    ESP_RETURN_ON_FALSE(classifier && classifier->calibration && sample && classification,
                        ESP_ERR_INVALID_ARG, TAG, "invalid classifier process arguments");
    memset(classification, 0, sizeof(*classification));
    classification->timestamp_ms = sample->timestamp_ms;

    if (!sample->left.valid || !sample->right.valid) {
        clear_filter_history(classifier);
        return ESP_OK;
    }

    const uint8_t index = classifier->write_index;
    classifier->left_x[index] = sample->left.x;
    classifier->left_y[index] = sample->left.y;
    classifier->left_z[index] = sample->left.z;
    classifier->right_x[index] = sample->right.x;
    classifier->right_y[index] = sample->right.y;
    classifier->right_z[index] = sample->right.z;
    classifier->write_index = (uint8_t)((index + 1) % classifier->calibration->filter_samples);
    if (classifier->sample_count < classifier->calibration->filter_samples) {
        classifier->sample_count++;
    }
    classification->filtered_samples = classifier->sample_count;
    if (classifier->sample_count < classifier->calibration->filter_samples) {
        return ESP_OK;
    }

    int32_t left_x = 0;
    int32_t left_y = 0;
    int32_t left_z = 0;
    int32_t right_x = 0;
    int32_t right_y = 0;
    int32_t right_z = 0;
    for (uint8_t i = 0; i < classifier->sample_count; ++i) {
        left_x += classifier->left_x[i];
        left_y += classifier->left_y[i];
        left_z += classifier->left_z[i];
        right_x += classifier->right_x[i];
        right_y += classifier->right_y[i];
        right_z += classifier->right_z[i];
    }
    left_x /= classifier->sample_count;
    left_y /= classifier->sample_count;
    left_z /= classifier->sample_count;
    right_x /= classifier->sample_count;
    right_y /= classifier->sample_count;
    right_z /= classifier->sample_count;

    if (!classifier->baseline_ready) {
        int16_t right_y_min = classifier->right_y[0];
        int16_t right_y_max = classifier->right_y[0];
        for (uint8_t i = 1; i < classifier->sample_count; ++i) {
            if (classifier->right_y[i] < right_y_min) {
                right_y_min = classifier->right_y[i];
            }
            if (classifier->right_y[i] > right_y_max) {
                right_y_max = classifier->right_y[i];
            }
        }
        const mosaico_edge_mask_t startup_edges = classify_axes(
            classifier->calibration, left_x, left_y, left_z, right_x, right_y,
            0, 0);
        const bool baseline_valid =
            right_y >= classifier->calibration->baseline_right_y_min &&
            right_y <= classifier->calibration->baseline_right_y_max &&
            (uint16_t)(right_y_max - right_y_min) <=
                classifier->calibration->baseline_right_y_max_span &&
            startup_edges == 0;
        if (!baseline_valid) {
            classifier->calibration_rejections++;
            classifier->calibration_state = classifier->calibration_rejections >= 3 ?
                MOSAICO_MAG_CALIBRATION_FAILED : MOSAICO_MAG_CALIBRATING;
            ESP_LOGW(TAG,
                     "baseline rejected: right_y=%ld span=%d edges=0x%02x attempts=%u",
                     (long)right_y, right_y_max - right_y_min, startup_edges,
                     classifier->calibration_rejections);
            clear_filter_history(classifier);
            return ESP_OK;
        }
        classifier->baseline_right_y = right_y;
        classifier->baseline_right_z = right_z;
        classifier->baseline_ready = true;
        classifier->calibration_state = MOSAICO_MAG_CALIBRATION_READY;
        ESP_LOGI(TAG, "baseline captured: right_y=%ld right_z=%ld samples=%u",
                 (long)classifier->baseline_right_y,
                 (long)classifier->baseline_right_z, classifier->sample_count);
        return ESP_OK;
    }

    const int32_t right_y_delta = right_y - classifier->baseline_right_y;
    const int32_t right_z_delta = right_z - classifier->baseline_right_z;

    classification->edge_mask = classify_axes(
        classifier->calibration, left_x, left_y, left_z, right_x, right_y,
        right_y_delta, right_z_delta);
    classification->saturated = value_is_saturated(classifier->calibration, left_x) ||
                                value_is_saturated(classifier->calibration, left_y) ||
                                value_is_saturated(classifier->calibration, left_z) ||
                                value_is_saturated(classifier->calibration, right_x) ||
                                value_is_saturated(classifier->calibration, right_y);
    classification->valid = true;
    return ESP_OK;
}

mosaico_edge_mask_t mosaico_edge_to_mask(mosaico_edge_t edge)
{
    switch (edge) {
    case MOSAICO_EDGE_TOP:    return MOSAICO_EDGE_MASK_TOP;
    case MOSAICO_EDGE_RIGHT:  return MOSAICO_EDGE_MASK_RIGHT;
    case MOSAICO_EDGE_BOTTOM: return MOSAICO_EDGE_MASK_BOTTOM;
    case MOSAICO_EDGE_LEFT:   return MOSAICO_EDGE_MASK_LEFT;
    default:                  return 0;
    }
}

mosaico_edge_t mosaico_edge_from_mask(mosaico_edge_mask_t mask)
{
    switch (mask) {
    case MOSAICO_EDGE_MASK_TOP:    return MOSAICO_EDGE_TOP;
    case MOSAICO_EDGE_MASK_RIGHT:  return MOSAICO_EDGE_RIGHT;
    case MOSAICO_EDGE_MASK_BOTTOM: return MOSAICO_EDGE_BOTTOM;
    case MOSAICO_EDGE_MASK_LEFT:   return MOSAICO_EDGE_LEFT;
    default:                       return MOSAICO_EDGE_NONE;
    }
}

static void emit_presence_event(
    mosaico_mag_presence_tracker_t *tracker,
    mosaico_mag_presence_event_type_t type,
    mosaico_edge_t edge,
    const mosaico_mag_classification_t *classification)
{
    const mosaico_mag_presence_event_t event = {
        .type = type,
        .edge = edge,
        .attached_mask = tracker->attached_mask,
        .saturated = classification->saturated,
        .timestamp_ms = classification->timestamp_ms,
    };
    ESP_LOGI(TAG, "presence event=%s edge=%s mask=0x%02x",
             type == MOSAICO_MAG_PRESENCE_CONTACT ? "CONTACT" : "RELEASE",
             mosaico_edge_to_string(edge), tracker->attached_mask);
    tracker->event_cb(&event, tracker->user_ctx);
}

esp_err_t mosaico_mag_presence_tracker_init(
    mosaico_mag_presence_tracker_t *tracker,
    const mosaico_mag_presence_config_t *config,
    mosaico_mag_presence_event_cb_t event_cb,
    void *user_ctx)
{
    ESP_RETURN_ON_FALSE(tracker && event_cb, ESP_ERR_INVALID_ARG, TAG,
                        "presence tracker or callback is null");
    const mosaico_mag_presence_config_t active =
        config ? *config : (mosaico_mag_presence_config_t)MOSAICO_MAG_PRESENCE_CONFIG_DEFAULT();
    ESP_RETURN_ON_FALSE(active.contact_confirm_frames > 0 && active.release_confirm_frames > 0,
                        ESP_ERR_INVALID_ARG, TAG, "presence confirmation frame count is zero");
    memset(tracker, 0, sizeof(*tracker));
    tracker->config = active;
    tracker->event_cb = event_cb;
    tracker->user_ctx = user_ctx;
    ESP_LOGI(TAG, "presence tracker initialized: contact=%u release=%u",
             active.contact_confirm_frames, active.release_confirm_frames);
    return ESP_OK;
}

void mosaico_mag_presence_tracker_reset(mosaico_mag_presence_tracker_t *tracker)
{
    if (!tracker) {
        return;
    }
    const mosaico_mag_presence_config_t config = tracker->config;
    const mosaico_mag_presence_event_cb_t event_cb = tracker->event_cb;
    void *user_ctx = tracker->user_ctx;
    memset(tracker, 0, sizeof(*tracker));
    tracker->config = config;
    tracker->event_cb = event_cb;
    tracker->user_ctx = user_ctx;
}

esp_err_t mosaico_mag_presence_tracker_process(
    mosaico_mag_presence_tracker_t *tracker,
    const mosaico_mag_classification_t *classification)
{
    ESP_RETURN_ON_FALSE(tracker && tracker->event_cb && classification,
                        ESP_ERR_INVALID_ARG, TAG, "invalid presence tracker arguments");
    const mosaico_edge_t edges[4] = {
        MOSAICO_EDGE_TOP,
        MOSAICO_EDGE_RIGHT,
        MOSAICO_EDGE_BOTTOM,
        MOSAICO_EDGE_LEFT,
    };
    const mosaico_edge_mask_t observed = classification->valid ?
        classification->edge_mask & MOSAICO_EDGE_MASK_ALL : 0;

    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        const mosaico_edge_mask_t bit = mosaico_edge_to_mask(edges[i]);
        const bool attached = (tracker->attached_mask & bit) != 0;
        const bool present = (observed & bit) != 0;
        if (!attached && present) {
            tracker->release_frames[i] = 0;
            if (tracker->contact_frames[i] < UINT8_MAX) {
                tracker->contact_frames[i]++;
            }
            if (tracker->contact_frames[i] >= tracker->config.contact_confirm_frames) {
                tracker->contact_frames[i] = 0;
                tracker->attached_mask |= bit;
                emit_presence_event(tracker, MOSAICO_MAG_PRESENCE_CONTACT, edges[i], classification);
            }
        } else if (attached && !present) {
            tracker->contact_frames[i] = 0;
            if (tracker->release_frames[i] < UINT8_MAX) {
                tracker->release_frames[i]++;
            }
            if (tracker->release_frames[i] >= tracker->config.release_confirm_frames) {
                tracker->release_frames[i] = 0;
                tracker->attached_mask &= (mosaico_edge_mask_t)~bit;
                emit_presence_event(tracker, MOSAICO_MAG_PRESENCE_RELEASE, edges[i], classification);
            }
        } else {
            tracker->contact_frames[i] = 0;
            tracker->release_frames[i] = 0;
        }
    }
    return ESP_OK;
}
