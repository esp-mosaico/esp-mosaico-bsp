/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pong_docking.h"

#include <inttypes.h>
#include <string.h>

#include "bsp/magnetometer.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mosaico_peer_session.h"
#include "pong_network.h"

static const char *TAG = "pong_docking";

typedef struct {
    bool started;
    bool hardware_available;
    bool sensors_valid;
    bool magnetometers_initialized;
    uint32_t last_sample_ms;
    pong_dock_state_t state;
    pong_role_t inferred_role;
    mosaico_edge_mask_t detected_edges;
    mosaico_edge_t local_edge;
    mosaico_edge_t peer_edge;
    uint64_t peer_id;
    uint32_t session_id;
    mosaico_mag_classifier_t classifier;
    mosaico_mag_presence_tracker_t presence;
    mosaico_peer_session_manager_t sessions;
} pong_docking_context_t;

static pong_docking_context_t s_docking;

static bool elapsed_at_least(uint32_t now_ms, uint32_t then_ms, uint32_t period_ms)
{
    return (uint32_t)(now_ms - then_ms) >= period_ms;
}

static void release_detected_edges(uint32_t now_ms)
{
    static const mosaico_edge_t edges[] = {
        MOSAICO_EDGE_TOP,
        MOSAICO_EDGE_RIGHT,
        MOSAICO_EDGE_BOTTOM,
        MOSAICO_EDGE_LEFT,
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        if ((s_docking.detected_edges & mosaico_edge_to_mask(edges[i])) != 0) {
            (void)mosaico_peer_session_local_release(
                &s_docking.sessions, edges[i], now_ms);
        }
    }
    s_docking.detected_edges = 0;
}

static void enter_wireless_mode(const char *reason, uint32_t now_ms)
{
    release_detected_edges(now_ms);
    s_docking.hardware_available = false;
    s_docking.sensors_valid = false;
    s_docking.state = PONG_DOCK_WIRELESS;
    s_docking.local_edge = MOSAICO_EDGE_NONE;
    s_docking.peer_edge = MOSAICO_EDGE_NONE;
    s_docking.peer_id = 0;
    s_docking.session_id = 0;
    (void)pong_network_send_layout(PONG_DOCK_WIRELESS);
    ESP_LOGW(TAG, "magnetic layout disabled; using wireless layout: reason=%s",
             reason);
}

static void presence_event_callback(const mosaico_mag_presence_event_t *event,
                                    void *user_ctx)
{
    (void)user_ctx;
    s_docking.detected_edges = event->attached_mask;
    esp_err_t ret;
    if (event->type == MOSAICO_MAG_PRESENCE_CONTACT) {
        ret = mosaico_peer_session_local_contact(
            &s_docking.sessions, event->edge, 0, event->timestamp_ms);
        if (s_docking.state == PONG_DOCK_WIRELESS) {
            s_docking.state = PONG_DOCK_ATTACHING;
            (void)pong_network_send_layout(s_docking.state);
        }
    } else {
        ret = mosaico_peer_session_local_release(
            &s_docking.sessions, event->edge, event->timestamp_ms);
        if (event->attached_mask == 0) {
            s_docking.state = PONG_DOCK_WIRELESS;
            (void)pong_network_send_layout(s_docking.state);
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "update local contact failed: edge=%d present=%d error=%s",
                 event->edge,
                 event->type == MOSAICO_MAG_PRESENCE_CONTACT,
                 esp_err_to_name(ret));
    }
}

static void session_event_callback(const mosaico_peer_session_event_t *event,
                                   void *user_ctx)
{
    (void)user_ctx;
    if (event->type == MOSAICO_PEER_SESSION_EVENT_SEND) {
        const esp_err_t ret = pong_network_send_peer_message(
            event->message_type, event->target_id, event->session_id,
            event->local_edge, event->peer_edge, event->relative_rotation);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "send contact session frame failed: type=%d error=%s",
                     event->message_type, esp_err_to_name(ret));
        }
        return;
    }

    if (event->type == MOSAICO_PEER_SESSION_EVENT_DETACHED) {
        if (s_docking.peer_id == event->peer_id &&
            s_docking.session_id == event->session_id) {
            s_docking.peer_id = 0;
            s_docking.session_id = 0;
            s_docking.local_edge = MOSAICO_EDGE_NONE;
            s_docking.peer_edge = MOSAICO_EDGE_NONE;
            s_docking.state = s_docking.detected_edges != 0 ?
                              PONG_DOCK_ATTACHING : PONG_DOCK_WIRELESS;
            (void)pong_network_send_layout(s_docking.state);
        }
        return;
    }

    pong_network_status_t network;
    if (pong_network_get_status(&network) != ESP_OK || !network.paired ||
        network.peer_id != event->peer_id) {
        ESP_LOGW(TAG, "reject contact layout from unpaired peer=%012" PRIx64,
                 event->peer_id);
        return;
    }

    s_docking.peer_id = event->peer_id;
    s_docking.session_id = event->session_id;
    s_docking.local_edge = event->local_edge;
    s_docking.peer_edge = event->peer_edge;
    bool valid_layout = false;
    if (event->local_edge == MOSAICO_EDGE_RIGHT &&
        event->peer_edge == MOSAICO_EDGE_LEFT) {
        s_docking.inferred_role = PONG_ROLE_LEFT;
        valid_layout = true;
    } else if (event->local_edge == MOSAICO_EDGE_LEFT &&
               event->peer_edge == MOSAICO_EDGE_RIGHT) {
        s_docking.inferred_role = PONG_ROLE_RIGHT;
        valid_layout = true;
    }
    s_docking.state = valid_layout && s_docking.inferred_role == network.local_role ?
                      PONG_DOCK_SEAMLESS : PONG_DOCK_REVERSED;
    (void)pong_network_send_layout(s_docking.state);
    ESP_LOGI(TAG, "layout attached: local=%d peer=%d state=%s",
             event->local_edge, event->peer_edge,
             s_docking.state == PONG_DOCK_SEAMLESS ? "SEAMLESS" : "REVERSED");
}

esp_err_t pong_docking_start(void)
{
    ESP_RETURN_ON_FALSE(!s_docking.started, ESP_ERR_INVALID_STATE, TAG,
                        "docking is already started");
    const uint64_t device_id = mosaico_peer_link_get_device_id();
    ESP_RETURN_ON_FALSE(device_id != 0, ESP_ERR_INVALID_STATE, TAG,
                        "peer link must be started before docking");

    memset(&s_docking, 0, sizeof(s_docking));
    s_docking.started = true;
    s_docking.state = PONG_DOCK_WIRELESS;
    s_docking.inferred_role = PONG_ROLE_LEFT;

    esp_err_t ret = mosaico_peer_session_init(
        &s_docking.sessions, device_id, NULL, session_event_callback, NULL);
    if (ret != ESP_OK) {
        s_docking.started = false;
        ESP_LOGE(TAG, "initialize contact sessions failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    ret = mosaico_mag_classifier_init(
        &s_docking.classifier, &MOSAICO_MAG_CALIBRATION_S31_V1);
    if (ret != ESP_OK) {
        enter_wireless_mode("classifier_init", 0);
        return ESP_OK;
    }
    ret = mosaico_mag_presence_tracker_init(
        &s_docking.presence, NULL, presence_event_callback, NULL);
    if (ret != ESP_OK) {
        enter_wireless_mode("presence_tracker_init", 0);
        return ESP_OK;
    }
    ret = bsp_magnetometer_init_all();
    if (ret != ESP_OK) {
        enter_wireless_mode("magnetometer_init", 0);
        return ESP_OK;
    }
    s_docking.magnetometers_initialized = true;
    s_docking.hardware_available = true;
    ESP_LOGI(TAG, "started magnetic layout detection");
    return ESP_OK;
}

void pong_docking_stop(void)
{
    if (!s_docking.started) {
        return;
    }
    release_detected_edges(s_docking.last_sample_ms);
    if (s_docking.magnetometers_initialized) {
        const esp_err_t right_ret =
            bsp_magnetometer_deinit(BSP_MAGNETOMETER_0);
        const esp_err_t left_ret =
            bsp_magnetometer_deinit(BSP_MAGNETOMETER_1);
        if (right_ret != ESP_OK || left_ret != ESP_OK) {
            ESP_LOGW(TAG, "deinitialize magnetometers failed: right=%s left=%s",
                     esp_err_to_name(right_ret), esp_err_to_name(left_ret));
        }
    }
    memset(&s_docking, 0, sizeof(s_docking));
}

void pong_docking_tick(uint32_t now_ms)
{
    if (!s_docking.started) {
        return;
    }
    mosaico_peer_session_tick(&s_docking.sessions, now_ms);
    if (!s_docking.hardware_available ||
        !elapsed_at_least(now_ms, s_docking.last_sample_ms,
                          PONG_DOCKING_SAMPLE_PERIOD_MS)) {
        return;
    }
    s_docking.last_sample_ms = now_ms;

    struct bmm150_mag_data right = {0};
    struct bmm150_mag_data left = {0};
    const esp_err_t right_ret =
        bsp_magnetometer_read(BSP_MAGNETOMETER_0, &right);
    const esp_err_t left_ret =
        bsp_magnetometer_read(BSP_MAGNETOMETER_1, &left);
    if (right_ret != ESP_OK || left_ret != ESP_OK) {
        ESP_LOGW(TAG, "read magnetometers failed: right=%s left=%s",
                 esp_err_to_name(right_ret), esp_err_to_name(left_ret));
        enter_wireless_mode("sensor_read", now_ms);
        return;
    }

    const mosaico_mag_sample_t sample = {
        .right = {
            .valid = true,
            .x = right.x,
            .y = right.y,
            .z = right.z,
        },
        .left = {
            .valid = true,
            .x = left.x,
            .y = left.y,
            .z = left.z,
        },
        .timestamp_ms = now_ms,
    };
    mosaico_mag_classification_t classification = {0};
    esp_err_t ret = mosaico_mag_classifier_process(
        &s_docking.classifier, &sample, &classification);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "classify magnetic sample failed: %s",
                 esp_err_to_name(ret));
        enter_wireless_mode("classifier_process", now_ms);
        return;
    }
    if (s_docking.classifier.calibration_state ==
        MOSAICO_MAG_CALIBRATION_FAILED) {
        enter_wireless_mode("calibration_failed", now_ms);
        return;
    }
    s_docking.sensors_valid = classification.valid;
    ret = mosaico_mag_presence_tracker_process(
        &s_docking.presence, &classification);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "track magnetic presence failed: %s",
                 esp_err_to_name(ret));
        enter_wireless_mode("presence_process", now_ms);
    }
}

void pong_docking_receive_peer_message(const mosaico_peer_message_t *message,
                                       uint32_t now_ms)
{
    if (!s_docking.started || !message ||
        message->type < MOSAICO_PEER_MSG_CONTACT_CLAIM ||
        message->type > MOSAICO_PEER_MSG_CONTACT_RELEASE) {
        return;
    }
    pong_network_status_t network;
    if (pong_network_get_status(&network) != ESP_OK || !network.paired ||
        message->source_id != network.peer_id) {
        return;
    }
    const esp_err_t ret =
        mosaico_peer_session_receive(&s_docking.sessions, message, now_ms);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "reject contact session frame: peer=%012" PRIx64
                 " type=%d error=%s", message->source_id, message->type,
                 esp_err_to_name(ret));
    }
}

esp_err_t pong_docking_get_status(pong_docking_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is null");
    ESP_RETURN_ON_FALSE(s_docking.started, ESP_ERR_INVALID_STATE, TAG,
                        "docking is not started");
    *status = (pong_docking_status_t) {
        .started = s_docking.started,
        .hardware_available = s_docking.hardware_available,
        .sensors_valid = s_docking.sensors_valid,
        .state = s_docking.state,
        .inferred_role = s_docking.inferred_role,
        .calibration_state = s_docking.classifier.calibration_state,
        .detected_edges = s_docking.detected_edges,
        .local_edge = s_docking.local_edge,
        .peer_edge = s_docking.peer_edge,
        .peer_id = s_docking.peer_id,
        .session_id = s_docking.session_id,
    };
    return ESP_OK;
}
