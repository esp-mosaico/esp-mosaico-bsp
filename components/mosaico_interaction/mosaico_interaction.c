/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_interaction.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "mosaico_interact";

static bool observation_qualifies(
    const mosaico_interaction_t *interaction,
    const mosaico_mag_observation_t *observation)
{
    return observation->valid &&
           observation->edge > MOSAICO_EDGE_NONE &&
           observation->edge < MOSAICO_EDGE_UNKNOWN &&
           observation->strength >= interaction->config.minimum_strength &&
           observation->confidence >= interaction->config.minimum_confidence;
}

static void emit_event(
    mosaico_interaction_t *interaction,
    mosaico_interaction_event_type_t type,
    mosaico_edge_t edge,
    mosaico_edge_t previous_edge,
    uint16_t rotation,
    uint16_t previous_rotation,
    const mosaico_mag_observation_t *observation)
{
    const mosaico_interaction_event_t event = {
        .type = type,
        .edge = edge,
        .previous_edge = previous_edge,
        .relative_rotation = rotation,
        .previous_rotation = previous_rotation,
        .strength = observation->strength,
        .confidence = observation->confidence,
        .saturated = observation->saturated,
        .timestamp_ms = observation->timestamp_ms,
    };
    ESP_LOGI(TAG, "event=%s edge=%s previous=%s rotation=%u confidence=%.2f",
             mosaico_interaction_event_to_string(type),
             mosaico_edge_to_string(edge),
             mosaico_edge_to_string(previous_edge),
             rotation,
             observation->confidence);
    interaction->event_cb(&event, interaction->user_ctx);
}

static int cardinal_index(mosaico_edge_t edge)
{
    switch (edge) {
    case MOSAICO_EDGE_TOP:    return 0;
    case MOSAICO_EDGE_RIGHT:  return 1;
    case MOSAICO_EDGE_BOTTOM: return 2;
    case MOSAICO_EDGE_LEFT:   return 3;
    default:                  return -1;
    }
}

static void emit_edge_transition(
    mosaico_interaction_t *interaction,
    const mosaico_mag_observation_t *observation)
{
    const mosaico_edge_t previous = interaction->attached_edge;
    const uint16_t previous_rotation = interaction->attached_rotation;
    interaction->attached_edge = observation->edge;
    interaction->attached_rotation = observation->relative_rotation;

    emit_event(interaction, MOSAICO_INTERACTION_EDGE_CHANGED,
               observation->edge, previous,
               observation->relative_rotation, previous_rotation, observation);

    const int old_index = cardinal_index(previous);
    const int new_index = cardinal_index(observation->edge);
    if (old_index < 0 || new_index < 0) {
        return;
    }
    const int step = (new_index - old_index + 4) % 4;
    if (step == 1) {
        emit_event(interaction, MOSAICO_INTERACTION_ORBIT_CW,
                   observation->edge, previous,
                   observation->relative_rotation, previous_rotation, observation);
    } else if (step == 3) {
        emit_event(interaction, MOSAICO_INTERACTION_ORBIT_CCW,
                   observation->edge, previous,
                   observation->relative_rotation, previous_rotation, observation);
    }
}

static void update_candidate(
    mosaico_interaction_t *interaction,
    const mosaico_mag_observation_t *observation)
{
    if (interaction->candidate_edge == observation->edge &&
        interaction->candidate_rotation == observation->relative_rotation) {
        if (interaction->candidate_frames < UINT8_MAX) {
            interaction->candidate_frames++;
        }
        return;
    }
    interaction->candidate_edge = observation->edge;
    interaction->candidate_rotation = observation->relative_rotation;
    interaction->candidate_frames = 1;
}

esp_err_t mosaico_interaction_init(
    mosaico_interaction_t *interaction,
    const mosaico_interaction_config_t *config,
    mosaico_interaction_event_cb_t event_cb,
    void *user_ctx)
{
    ESP_RETURN_ON_FALSE(interaction && event_cb, ESP_ERR_INVALID_ARG, TAG,
                        "interaction or event callback is null");
    const mosaico_interaction_config_t active =
        config ? *config : (mosaico_interaction_config_t)MOSAICO_INTERACTION_CONFIG_DEFAULT();
    ESP_RETURN_ON_FALSE(active.approach_confirm_frames > 0 &&
                        active.contact_confirm_frames > 0 &&
                        active.release_confirm_frames > 0,
                        ESP_ERR_INVALID_ARG, TAG, "confirmation frame count is zero");
    memset(interaction, 0, sizeof(*interaction));
    interaction->config = active;
    interaction->event_cb = event_cb;
    interaction->user_ctx = user_ctx;
    ESP_LOGI(TAG, "initialized: approach=%u contact=%u release=%u",
             active.approach_confirm_frames,
             active.contact_confirm_frames,
             active.release_confirm_frames);
    return ESP_OK;
}

void mosaico_interaction_reset(mosaico_interaction_t *interaction)
{
    if (!interaction) {
        return;
    }
    const mosaico_interaction_config_t config = interaction->config;
    const mosaico_interaction_event_cb_t event_cb = interaction->event_cb;
    void *user_ctx = interaction->user_ctx;
    memset(interaction, 0, sizeof(*interaction));
    interaction->config = config;
    interaction->event_cb = event_cb;
    interaction->user_ctx = user_ctx;
}

esp_err_t mosaico_interaction_process(
    mosaico_interaction_t *interaction,
    const mosaico_mag_observation_t *observation)
{
    ESP_RETURN_ON_FALSE(interaction && observation && interaction->event_cb,
                        ESP_ERR_INVALID_ARG, TAG, "invalid process arguments");
    const bool qualified = observation_qualifies(interaction, observation);

    switch (interaction->state) {
    case MOSAICO_INTERACTION_STATE_IDLE:
        if (!qualified) {
            interaction->candidate_frames = 0;
            return ESP_OK;
        }
        update_candidate(interaction, observation);
        if (interaction->candidate_frames >= interaction->config.approach_confirm_frames) {
            interaction->state = MOSAICO_INTERACTION_STATE_APPROACH;
            interaction->candidate_frames = 0;
            emit_event(interaction, MOSAICO_INTERACTION_APPROACH,
                       observation->edge, MOSAICO_EDGE_NONE,
                       observation->relative_rotation, 0, observation);
        }
        break;

    case MOSAICO_INTERACTION_STATE_APPROACH:
        if (!qualified) {
            interaction->state = MOSAICO_INTERACTION_STATE_IDLE;
            interaction->candidate_frames = 0;
            return ESP_OK;
        }
        update_candidate(interaction, observation);
        if (interaction->candidate_frames >= interaction->config.contact_confirm_frames) {
            interaction->state = MOSAICO_INTERACTION_STATE_ATTACHED;
            interaction->attached_edge = observation->edge;
            interaction->attached_rotation = observation->relative_rotation;
            interaction->candidate_frames = 0;
            emit_event(interaction, MOSAICO_INTERACTION_CONTACT,
                       observation->edge, MOSAICO_EDGE_NONE,
                       observation->relative_rotation, 0, observation);
        }
        break;

    case MOSAICO_INTERACTION_STATE_ATTACHED:
        if (!qualified) {
            if (interaction->release_frames < UINT8_MAX) {
                interaction->release_frames++;
            }
            if (interaction->release_frames >= interaction->config.release_confirm_frames) {
                const mosaico_edge_t previous = interaction->attached_edge;
                const uint16_t previous_rotation = interaction->attached_rotation;
                emit_event(interaction, MOSAICO_INTERACTION_RELEASE,
                           MOSAICO_EDGE_NONE, previous, 0, previous_rotation, observation);
                mosaico_interaction_reset(interaction);
            }
            return ESP_OK;
        }

        interaction->release_frames = 0;
        if (observation->edge == interaction->attached_edge &&
            observation->relative_rotation == interaction->attached_rotation) {
            interaction->candidate_frames = 0;
            return ESP_OK;
        }

        update_candidate(interaction, observation);
        if (interaction->candidate_frames < interaction->config.contact_confirm_frames) {
            return ESP_OK;
        }
        interaction->candidate_frames = 0;
        if (observation->edge != interaction->attached_edge) {
            emit_edge_transition(interaction, observation);
        } else {
            const uint16_t previous_rotation = interaction->attached_rotation;
            interaction->attached_rotation = observation->relative_rotation;
            emit_event(interaction, MOSAICO_INTERACTION_ORIENTATION_CHANGED,
                       observation->edge, observation->edge,
                       observation->relative_rotation, previous_rotation, observation);
        }
        break;

    default:
        ESP_LOGE(TAG, "invalid state=%d", interaction->state);
        mosaico_interaction_reset(interaction);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

const char *mosaico_edge_to_string(mosaico_edge_t edge)
{
    static const char *const names[] = {
        [MOSAICO_EDGE_NONE] = "NONE",
        [MOSAICO_EDGE_TOP] = "TOP",
        [MOSAICO_EDGE_RIGHT] = "RIGHT",
        [MOSAICO_EDGE_BOTTOM] = "BOTTOM",
        [MOSAICO_EDGE_LEFT] = "LEFT",
        [MOSAICO_EDGE_CORNER_TL] = "CORNER_TL",
        [MOSAICO_EDGE_CORNER_TR] = "CORNER_TR",
        [MOSAICO_EDGE_CORNER_BR] = "CORNER_BR",
        [MOSAICO_EDGE_CORNER_BL] = "CORNER_BL",
        [MOSAICO_EDGE_UNKNOWN] = "UNKNOWN",
    };
    return edge <= MOSAICO_EDGE_UNKNOWN && names[edge] ? names[edge] : "INVALID";
}

const char *mosaico_interaction_event_to_string(mosaico_interaction_event_type_t type)
{
    static const char *const names[] = {
        [MOSAICO_INTERACTION_APPROACH] = "APPROACH",
        [MOSAICO_INTERACTION_CONTACT] = "CONTACT",
        [MOSAICO_INTERACTION_RELEASE] = "RELEASE",
        [MOSAICO_INTERACTION_EDGE_CHANGED] = "EDGE_CHANGED",
        [MOSAICO_INTERACTION_ORBIT_CW] = "ORBIT_CW",
        [MOSAICO_INTERACTION_ORBIT_CCW] = "ORBIT_CCW",
        [MOSAICO_INTERACTION_ORIENTATION_CHANGED] = "ORIENTATION_CHANGED",
    };
    return type <= MOSAICO_INTERACTION_ORIENTATION_CHANGED && names[type] ? names[type] : "INVALID";
}
