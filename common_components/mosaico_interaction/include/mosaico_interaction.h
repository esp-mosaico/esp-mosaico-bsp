/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOSAICO_EDGE_NONE = 0,
    MOSAICO_EDGE_TOP,
    MOSAICO_EDGE_RIGHT,
    MOSAICO_EDGE_BOTTOM,
    MOSAICO_EDGE_LEFT,
    MOSAICO_EDGE_CORNER_TL,
    MOSAICO_EDGE_CORNER_TR,
    MOSAICO_EDGE_CORNER_BR,
    MOSAICO_EDGE_CORNER_BL,
    MOSAICO_EDGE_UNKNOWN,
} mosaico_edge_t;

typedef enum {
    MOSAICO_INTERACTION_APPROACH = 0,
    MOSAICO_INTERACTION_CONTACT,
    MOSAICO_INTERACTION_RELEASE,
    MOSAICO_INTERACTION_EDGE_CHANGED,
    MOSAICO_INTERACTION_ORBIT_CW,
    MOSAICO_INTERACTION_ORBIT_CCW,
    MOSAICO_INTERACTION_ORIENTATION_CHANGED,
} mosaico_interaction_event_type_t;

/**
 * A classifier output for one instant. Sources that provide a single edge,
 * rotation, strength, and confidence feed this interface.
 */
typedef struct {
    mosaico_edge_t edge;
    uint16_t relative_rotation;
    float strength;
    float confidence;
    bool valid;
    bool saturated;
    uint32_t timestamp_ms;
} mosaico_mag_observation_t;

typedef struct {
    mosaico_interaction_event_type_t type;
    mosaico_edge_t edge;
    mosaico_edge_t previous_edge;
    uint16_t relative_rotation;
    uint16_t previous_rotation;
    float strength;
    float confidence;
    bool saturated;
    uint32_t timestamp_ms;
} mosaico_interaction_event_t;

typedef struct {
    uint8_t approach_confirm_frames;
    uint8_t contact_confirm_frames;
    uint8_t release_confirm_frames;
    float minimum_strength;
    float minimum_confidence;
} mosaico_interaction_config_t;

#define MOSAICO_INTERACTION_CONFIG_DEFAULT() { \
    .approach_confirm_frames = 2,               \
    .contact_confirm_frames = 3,                \
    .release_confirm_frames = 3,                \
    .minimum_strength = 0.5f,                   \
    .minimum_confidence = 0.5f,                 \
}

typedef enum {
    MOSAICO_INTERACTION_STATE_IDLE = 0,
    MOSAICO_INTERACTION_STATE_APPROACH,
    MOSAICO_INTERACTION_STATE_ATTACHED,
} mosaico_interaction_state_t;

typedef void (*mosaico_interaction_event_cb_t)(
    const mosaico_interaction_event_t *event,
    void *user_ctx);

typedef struct {
    mosaico_interaction_config_t config;
    mosaico_interaction_event_cb_t event_cb;
    void *user_ctx;
    mosaico_interaction_state_t state;
    mosaico_edge_t candidate_edge;
    mosaico_edge_t attached_edge;
    uint16_t candidate_rotation;
    uint16_t attached_rotation;
    uint8_t candidate_frames;
    uint8_t release_frames;
} mosaico_interaction_t;

esp_err_t mosaico_interaction_init(
    mosaico_interaction_t *interaction,
    const mosaico_interaction_config_t *config,
    mosaico_interaction_event_cb_t event_cb,
    void *user_ctx);

void mosaico_interaction_reset(mosaico_interaction_t *interaction);

esp_err_t mosaico_interaction_process(
    mosaico_interaction_t *interaction,
    const mosaico_mag_observation_t *observation);

const char *mosaico_edge_to_string(mosaico_edge_t edge);
const char *mosaico_interaction_event_to_string(mosaico_interaction_event_type_t type);

#ifdef __cplusplus
}
#endif
