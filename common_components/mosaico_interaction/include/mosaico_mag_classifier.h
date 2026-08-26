/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosaico_interaction.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_MAG_FILTER_MAX_SAMPLES 8U

typedef uint8_t mosaico_edge_mask_t;

#define MOSAICO_EDGE_MASK_TOP    (1U << 0)
#define MOSAICO_EDGE_MASK_RIGHT  (1U << 1)
#define MOSAICO_EDGE_MASK_BOTTOM (1U << 2)
#define MOSAICO_EDGE_MASK_LEFT   (1U << 3)
#define MOSAICO_EDGE_MASK_ALL    (MOSAICO_EDGE_MASK_TOP | MOSAICO_EDGE_MASK_RIGHT | \
                                  MOSAICO_EDGE_MASK_BOTTOM | MOSAICO_EDGE_MASK_LEFT)

typedef struct {
    bool valid;
    int16_t x;
    int16_t y;
    int16_t z;
} mosaico_mag_channel_sample_t;

/**
 * One synchronized sample from the two populated BMM150 positions.
 *
 * The ESP-Mosaico V1 profile uses right=0x11 and left=0x12. Keeping the
 * physical positions in this interface prevents BSP indices from leaking into
 * the hardware-independent classifier.
 */
typedef struct {
    mosaico_mag_channel_sample_t right;
    mosaico_mag_channel_sample_t left;
    uint32_t timestamp_ms;
} mosaico_mag_sample_t;

typedef struct {
    const char *name;
    uint8_t filter_samples;
    int16_t saturation_limit;
    int16_t left_reject_z;
    int16_t left_y_high;
    int16_t down_y_both_sides;
    int16_t down_y_with_side;
    int16_t down_y_without_side;
    int16_t down_right_y_without_side;
    /* Standalone vertical-edge thresholds relative to the startup right-Y baseline. */
    int16_t down_right_y_delta;
    int16_t up_right_y;
    int16_t up_right_y_delta;
    int16_t up_right_y_high;
    int16_t up_right_y_with_down;
    int16_t up_right_y_both_with_down;
    int16_t up_right_z_delta_with_down;
    int16_t up_right_y_fallback;
    int16_t up_left_z_with_down;
    int16_t baseline_right_y_min;
    int16_t baseline_right_y_max;
    uint16_t baseline_right_y_max_span;
} mosaico_mag_calibration_t;

/** First prototype profile derived from the mag_tile_collect captures. */
extern const mosaico_mag_calibration_t MOSAICO_MAG_CALIBRATION_S31_V1;

typedef struct {
    mosaico_edge_mask_t edge_mask;
    bool valid;
    bool saturated;
    uint8_t filtered_samples;
    uint32_t timestamp_ms;
} mosaico_mag_classification_t;

typedef enum {
    MOSAICO_MAG_CALIBRATING = 0,
    MOSAICO_MAG_CALIBRATION_READY,
    MOSAICO_MAG_CALIBRATION_FAILED,
} mosaico_mag_calibration_state_t;

typedef struct {
    const mosaico_mag_calibration_t *calibration;
    int16_t left_x[MOSAICO_MAG_FILTER_MAX_SAMPLES];
    int16_t left_y[MOSAICO_MAG_FILTER_MAX_SAMPLES];
    int16_t left_z[MOSAICO_MAG_FILTER_MAX_SAMPLES];
    int16_t right_x[MOSAICO_MAG_FILTER_MAX_SAMPLES];
    int16_t right_y[MOSAICO_MAG_FILTER_MAX_SAMPLES];
    int16_t right_z[MOSAICO_MAG_FILTER_MAX_SAMPLES];
    uint8_t write_index;
    uint8_t sample_count;
    /* Captured from the first complete filter window after initialization/reset. */
    int32_t baseline_right_y;
    int32_t baseline_right_z;
    bool baseline_ready;
    uint8_t calibration_rejections;
    mosaico_mag_calibration_state_t calibration_state;
} mosaico_mag_classifier_t;

esp_err_t mosaico_mag_classifier_init(
    mosaico_mag_classifier_t *classifier,
    const mosaico_mag_calibration_t *calibration);

void mosaico_mag_classifier_reset(mosaico_mag_classifier_t *classifier);

esp_err_t mosaico_mag_classifier_process(
    mosaico_mag_classifier_t *classifier,
    const mosaico_mag_sample_t *sample,
    mosaico_mag_classification_t *classification);

typedef enum {
    MOSAICO_MAG_PRESENCE_CONTACT = 0,
    MOSAICO_MAG_PRESENCE_RELEASE,
} mosaico_mag_presence_event_type_t;

typedef struct {
    mosaico_mag_presence_event_type_t type;
    mosaico_edge_t edge;
    mosaico_edge_mask_t attached_mask;
    bool saturated;
    uint32_t timestamp_ms;
} mosaico_mag_presence_event_t;

typedef void (*mosaico_mag_presence_event_cb_t)(
    const mosaico_mag_presence_event_t *event,
    void *user_ctx);

typedef struct {
    uint8_t contact_confirm_frames;
    uint8_t release_confirm_frames;
} mosaico_mag_presence_config_t;

#define MOSAICO_MAG_PRESENCE_CONFIG_DEFAULT() { \
    .contact_confirm_frames = 3,                \
    .release_confirm_frames = 3,                \
}

typedef struct {
    mosaico_mag_presence_config_t config;
    mosaico_mag_presence_event_cb_t event_cb;
    void *user_ctx;
    mosaico_edge_mask_t attached_mask;
    uint8_t contact_frames[4];
    uint8_t release_frames[4];
} mosaico_mag_presence_tracker_t;

esp_err_t mosaico_mag_presence_tracker_init(
    mosaico_mag_presence_tracker_t *tracker,
    const mosaico_mag_presence_config_t *config,
    mosaico_mag_presence_event_cb_t event_cb,
    void *user_ctx);

void mosaico_mag_presence_tracker_reset(mosaico_mag_presence_tracker_t *tracker);

esp_err_t mosaico_mag_presence_tracker_process(
    mosaico_mag_presence_tracker_t *tracker,
    const mosaico_mag_classification_t *classification);

mosaico_edge_mask_t mosaico_edge_to_mask(mosaico_edge_t edge);
mosaico_edge_t mosaico_edge_from_mask(mosaico_edge_mask_t mask);

#ifdef __cplusplus
}
#endif
