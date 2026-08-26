/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pong_game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pong_docking.h"
#include "pong_feedback.h"
#include "pong_input.h"
#include "pong_network.h"
#include "pong_physics.h"

#define GAME_TASK_STACK_SIZE       8192U
#define GAME_TASK_PRIORITY         7U
#define ROUND_RESULT_HOLD_MS       1200U
#define COUNTDOWN_DURATION_MS      3000U
#define CLIENT_EXTRAPOLATION_MS    100U
#define PHYSICS_STEP_US             (1000000U / PONG_PHYSICS_HZ)
#define MAX_PHYSICS_CATCH_UP_STEPS  3U
#define CLIENT_BALL_CORRECTION      0.28f
#define CLIENT_REMOTE_CORRECTION    0.20f
#define CLIENT_LOCAL_CORRECTION     0.035f
#define CLIENT_LARGE_ERROR_CORRECTION 0.18f
#define CLIENT_LARGE_ERROR_PIXELS   48.0f

static const char *TAG = "pong_game";

typedef struct {
    bool running;
    bool input_ready;
    bool reported_input_ready;
    bool have_snapshot;
    uint16_t previous_remote_buttons;
    uint32_t phase_started_ms;
    uint32_t last_snapshot_ms;
    uint32_t last_client_update_ms;
    uint32_t last_feedback_event_id;
    TaskHandle_t task;
    SemaphoreHandle_t render_lock;
    pong_input_handle_t input;
    pong_input_t local_input;
    pong_input_t remote_input;
    pong_physics_t physics;
    pong_world_t client_world;
    pong_world_t target_world;
    pong_render_snapshot_t render;
} pong_game_context_t;

static pong_game_context_t s_game;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static float clampf(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

static void post_feedback_event(pong_event_kind_t event)
{
    const esp_err_t err = pong_feedback_post(event);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "Feedback event dropped: event=%u error=%s",
                 (unsigned)event, esp_err_to_name(err));
    }
}

static void post_feedback_cue(pong_feedback_cue_t cue)
{
    const esp_err_t err = pong_feedback_post_cue(cue);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "Feedback cue dropped: cue=%u error=%s",
                 (unsigned)cue, esp_err_to_name(err));
    }
}

static void start_countdown(uint32_t timestamp_ms)
{
    s_game.physics.world.phase = PONG_PHASE_COUNTDOWN;
    s_game.physics.world.countdown_ms = COUNTDOWN_DURATION_MS;
    s_game.physics.world.ball.position =
        (pong_vec2_t) { PONG_WORLD_WIDTH * 0.5f, PONG_WORLD_HEIGHT * 0.5f };
    s_game.physics.world.ball.velocity = (pong_vec2_t) {0};
    s_game.physics.world.ball.spin = 0.0f;
    s_game.phase_started_ms = timestamp_ms;
}

static void reset_to_lobby(void)
{
    const uint32_t seed = s_game.physics.prng_state;
    pong_physics_init(&s_game.physics, seed);
    s_game.physics.world.phase = s_game.input_ready ?
                                 PONG_PHASE_LOBBY : PONG_PHASE_CALIBRATING;
    s_game.have_snapshot = false;
    s_game.last_client_update_ms = 0;
    s_game.previous_remote_buttons = 0;
    (void)pong_network_set_ready(false);
}

static void handle_network_events(uint32_t timestamp_ms,
                                  const pong_network_status_t *status)
{
    pong_network_event_t event;
    while (pong_network_poll_event(&event)) {
        switch (event.kind) {
        case PONG_NETWORK_EVENT_PAIRED:
            s_game.physics.world.phase = s_game.input_ready ?
                                         PONG_PHASE_LOBBY :
                                         PONG_PHASE_CALIBRATING;
            post_feedback_cue(PONG_FEEDBACK_CUE_PAIRED);
            break;
        case PONG_NETWORK_EVENT_INPUT:
            s_game.remote_input = event.data.input;
            if (status->is_host) {
                pong_physics_set_input(&s_game.physics, PONG_ROLE_RIGHT,
                                       &s_game.remote_input);
            }
            break;
        case PONG_NETWORK_EVENT_SNAPSHOT:
            if (!status->is_host) {
                s_game.target_world = event.data.snapshot;
                if (!s_game.have_snapshot) {
                    s_game.client_world = s_game.target_world;
                    s_game.have_snapshot = true;
                }
                s_game.last_snapshot_ms = timestamp_ms;
            }
            break;
        case PONG_NETWORK_EVENT_PEER_PAUSED:
            if (status->is_host) {
                s_game.physics.world.phase = PONG_PHASE_PAUSED;
            }
            post_feedback_cue(PONG_FEEDBACK_CUE_PAUSED);
            break;
        case PONG_NETWORK_EVENT_PEER_RESUMED:
            if (status->is_host && s_game.input_ready &&
                status->local_ready && status->peer_ready) {
                start_countdown(timestamp_ms);
            }
            post_feedback_cue(PONG_FEEDBACK_CUE_RESUMED);
            break;
        case PONG_NETWORK_EVENT_PEER_LOST:
        case PONG_NETWORK_EVENT_PEER_REBOOT:
            post_feedback_cue(PONG_FEEDBACK_CUE_CONNECTION_LOST);
            reset_to_lobby();
            break;
        case PONG_NETWORK_EVENT_CONTROL:
            if (event.data.control.action == PONG_CONTROL_EMOTE) {
                post_feedback_cue(PONG_FEEDBACK_CUE_EMOTE);
                break;
            }
            if (status->is_host) {
                if (event.data.control.action == PONG_CONTROL_PAUSE) {
                    s_game.physics.world.phase = PONG_PHASE_PAUSED;
                    post_feedback_cue(PONG_FEEDBACK_CUE_PAUSED);
                } else if (event.data.control.action == PONG_CONTROL_RESUME &&
                           s_game.input_ready && status->local_ready &&
                           status->peer_ready) {
                    start_countdown(timestamp_ms);
                    post_feedback_cue(PONG_FEEDBACK_CUE_RESUMED);
                } else if (event.data.control.action ==
                           PONG_CONTROL_RESET_MATCH) {
                    const uint32_t seed = s_game.physics.prng_state;
                    pong_physics_init(&s_game.physics, seed);
                    start_countdown(timestamp_ms);
                }
            }
            break;
        default:
            break;
        }
    }
}

static bool serving_player_pressed_b(const pong_network_status_t *status,
                                     uint16_t local_edges)
{
    const pong_role_t serving =
        (pong_role_t)s_game.physics.world.serving_role;
    if (serving == status->local_role) {
        return (local_edges & PONG_BUTTON_CONFIRM) != 0;
    }
    const uint16_t edges = s_game.remote_input.buttons &
                           (uint16_t)~s_game.previous_remote_buttons;
    return (edges & PONG_BUTTON_CONFIRM) != 0;
}

static void run_host(uint32_t timestamp_ms,
                     const pong_network_status_t *status,
                     uint16_t local_edges,
                     uint32_t physics_steps)
{
    pong_physics_set_input(&s_game.physics, status->local_role,
                           &s_game.local_input);
    pong_physics_set_input(&s_game.physics, PONG_ROLE_RIGHT,
                           status->local_role == PONG_ROLE_RIGHT ?
                           &s_game.local_input : &s_game.remote_input);

    pong_world_t *world = &s_game.physics.world;
    if (!s_game.input_ready || status->paused) {
        world->phase = PONG_PHASE_PAUSED;
    } else if (!status->local_ready || !status->peer_ready) {
        world->phase = PONG_PHASE_LOBBY;
    } else if (world->phase == PONG_PHASE_LOBBY ||
               world->phase == PONG_PHASE_CALIBRATING) {
        start_countdown(timestamp_ms);
    }

    if (world->phase == PONG_PHASE_COUNTDOWN) {
        const uint32_t elapsed = timestamp_ms - s_game.phase_started_ms;
        world->countdown_ms = elapsed >= COUNTDOWN_DURATION_MS ?
                              0 : COUNTDOWN_DURATION_MS - elapsed;
        if (world->countdown_ms == 0 &&
            serving_player_pressed_b(status, local_edges)) {
            pong_physics_start_round(&s_game.physics,
                (pong_role_t)world->serving_role);
        }
    } else if (world->phase == PONG_PHASE_ROUND_OVER &&
               timestamp_ms - s_game.phase_started_ms >=
                   ROUND_RESULT_HOLD_MS) {
        start_countdown(timestamp_ms);
    } else if (world->phase == PONG_PHASE_MATCH_OVER &&
               (local_edges & PONG_BUTTON_RESTART) != 0) {
        const uint32_t seed = s_game.physics.prng_state;
        pong_physics_init(&s_game.physics, seed);
        start_countdown(timestamp_ms);
    }

    for (uint32_t step = 0; step < physics_steps; ++step) {
        const pong_phase_t phase_before = world->phase;
        pong_physics_step(&s_game.physics);
        if (phase_before == PONG_PHASE_PLAYING &&
            world->phase == PONG_PHASE_ROUND_OVER) {
            s_game.phase_started_ms = timestamp_ms;
        }
    }
    if (world->event_id != s_game.last_feedback_event_id) {
        s_game.last_feedback_event_id = world->event_id;
        post_feedback_event(world->event);
    }
    (void)pong_network_send_snapshot(world);
    s_game.previous_remote_buttons = s_game.remote_input.buttons;
}

static void run_client(uint32_t timestamp_ms,
                       const pong_network_status_t *status)
{
    (void)pong_network_send_input(&s_game.local_input);
    if (!s_game.have_snapshot) {
        return;
    }

    float delta_seconds = PONG_PHYSICS_STEP_SECONDS;
    if (s_game.last_client_update_ms != 0U) {
        delta_seconds = clampf(
            (float)(timestamp_ms - s_game.last_client_update_ms) / 1000.0f,
            0.001f, 0.032f);
    }
    s_game.last_client_update_ms = timestamp_ms;

    const pong_world_t previous = s_game.client_world;
    pong_world_t target = s_game.target_world;
    if (target.phase == PONG_PHASE_PLAYING) {
        const uint32_t extrapolation_ms = (uint32_t)clampf(
            (float)(timestamp_ms - s_game.last_snapshot_ms), 0.0f,
            CLIENT_EXTRAPOLATION_MS);
        const float extrapolation = (float)extrapolation_ms / 1000.0f;
        for (size_t i = 0; i < 2; ++i) {
            target.paddles[i].y = clampf(
                target.paddles[i].y +
                    target.paddles[i].velocity * extrapolation,
                PONG_PADDLE_HEIGHT * 0.5f,
                PONG_WORLD_HEIGHT - PONG_PADDLE_HEIGHT * 0.5f);
        }
        const uint32_t prediction_steps =
            (extrapolation_ms * PONG_PHYSICS_HZ + 500U) / 1000U;
        pong_physics_predict_ball(&target, prediction_steps, &target.ball);
    }
    s_game.client_world = target;
    if (target.phase == PONG_PHASE_PLAYING &&
        previous.phase == PONG_PHASE_PLAYING) {
        s_game.client_world.ball.position.x =
            target.ball.position.x * CLIENT_BALL_CORRECTION +
            previous.ball.position.x * (1.0f - CLIENT_BALL_CORRECTION);
        s_game.client_world.ball.position.y =
            target.ball.position.y * CLIENT_BALL_CORRECTION +
            previous.ball.position.y * (1.0f - CLIENT_BALL_CORRECTION);

        const pong_role_t remote_role = status->local_role == PONG_ROLE_LEFT ?
                                        PONG_ROLE_RIGHT : PONG_ROLE_LEFT;
        s_game.client_world.paddles[remote_role].y =
            target.paddles[remote_role].y * CLIENT_REMOTE_CORRECTION +
            previous.paddles[remote_role].y *
                (1.0f - CLIENT_REMOTE_CORRECTION);
    }

    pong_paddle_t *local = &s_game.client_world.paddles[status->local_role];
    const float predicted_y = clampf(
        previous.paddles[status->local_role].y +
            pong_physics_paddle_velocity(s_game.local_input.axis_y) *
                delta_seconds,
        PONG_PADDLE_HEIGHT * 0.5f,
        PONG_WORLD_HEIGHT - PONG_PADDLE_HEIGHT * 0.5f);
    const float local_error = target.paddles[status->local_role].y - predicted_y;
    const float correction = fabsf(local_error) > CLIENT_LARGE_ERROR_PIXELS ?
                             CLIENT_LARGE_ERROR_CORRECTION :
                             CLIENT_LOCAL_CORRECTION;
    local->y = clampf(predicted_y + local_error * correction,
                      PONG_PADDLE_HEIGHT * 0.5f,
                      PONG_WORLD_HEIGHT - PONG_PADDLE_HEIGHT * 0.5f);
    local->velocity = (local->y - previous.paddles[status->local_role].y) /
                      delta_seconds;
    local->tilt = s_game.local_input.axis_x;
    if (s_game.client_world.event_id != s_game.last_feedback_event_id) {
        s_game.last_feedback_event_id = s_game.client_world.event_id;
        post_feedback_event(s_game.client_world.event);
    }
}

static void publish_render(const pong_network_status_t *network)
{
    pong_docking_status_t docking = {
        .state = PONG_DOCK_WIRELESS,
    };
    (void)pong_docking_get_status(&docking);

    pong_render_snapshot_t next = {
        .world = network->is_host ? s_game.physics.world : s_game.client_world,
        .local_role = network->local_role,
        .dock_state = docking.state,
        .joystick_ready = s_game.input_ready,
        .peer_present = network->paired,
        .local_ready = network->local_ready,
        .peer_ready = network->peer_ready,
        .is_host = network->is_host,
        .latency_ms = network->rtt_ms / 2U,
        .rssi = network->rssi,
        .packet_loss_percent = network->packet_loss_percent,
    };
    if (network->paired) {
        snprintf(next.peer_label, sizeof(next.peer_label), "%012llX",
                 (unsigned long long)network->peer_id);
    } else {
        snprintf(next.peer_label, sizeof(next.peer_label), "SEARCHING");
    }
    if (!s_game.input_ready) {
        snprintf(next.status, sizeof(next.status),
                 "ROTATE JOYSTICK, THEN RELEASE CENTER");
    } else if (!network->paired) {
        snprintf(next.status, sizeof(next.status), "SEARCHING FOR PLAYER");
    } else if (!network->local_ready) {
        snprintf(next.status, sizeof(next.status), "PRESS 1 TO READY");
    } else if (!network->peer_ready) {
        snprintf(next.status, sizeof(next.status), "WAITING FOR PEER");
    } else if (next.world.phase == PONG_PHASE_COUNTDOWN &&
               next.world.countdown_ms == 0) {
        snprintf(next.status, sizeof(next.status), "SERVER: PRESS 1");
    } else if (network->paused) {
        snprintf(next.status, sizeof(next.status), "CONNECTION PAUSED");
    } else {
        snprintf(next.status, sizeof(next.status),
                 docking.state == PONG_DOCK_SEAMLESS ?
                 "SEAMLESS COURT" : "WIRELESS PORTAL");
    }

    xSemaphoreTake(s_game.render_lock, portMAX_DELAY);
    s_game.render = next;
    xSemaphoreGive(s_game.render_lock);
}

static void game_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_physics_us = esp_timer_get_time();
    uint32_t physics_accumulator_us = PHYSICS_STEP_US;
    uint32_t step_index = 0;
    while (s_game.running) {
        const int64_t current_us = esp_timer_get_time();
        uint32_t elapsed_us = (uint32_t)(current_us - last_physics_us);
        last_physics_us = current_us;
        const uint32_t maximum_accumulator_us =
            PHYSICS_STEP_US * MAX_PHYSICS_CATCH_UP_STEPS;
        if (elapsed_us > maximum_accumulator_us) {
            elapsed_us = maximum_accumulator_us;
        }
        physics_accumulator_us += elapsed_us;
        if (physics_accumulator_us > maximum_accumulator_us) {
            physics_accumulator_us = maximum_accumulator_us;
        }
        const uint32_t physics_steps =
            physics_accumulator_us / PHYSICS_STEP_US;
        physics_accumulator_us -= physics_steps * PHYSICS_STEP_US;

        const uint32_t timestamp_ms = now_ms();
        pong_network_tick(timestamp_ms);
        pong_docking_tick(timestamp_ms);

        (void)pong_input_poll(s_game.input, timestamp_ms,
                              &s_game.local_input, &s_game.input_ready);
        const uint16_t edges = pong_input_pressed_edges(s_game.input);

        pong_network_status_t network = {0};
        if (pong_network_get_status(&network) != ESP_OK) {
            const uint32_t delay_ms = (++step_index % 3U) == 0U ? 9U : 8U;
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(delay_ms));
            continue;
        }
        handle_network_events(timestamp_ms, &network);
        (void)pong_network_get_status(&network);

        if (network.paired &&
            s_game.input_ready != s_game.reported_input_ready) {
            (void)pong_network_send_control(
                s_game.input_ready ? PONG_CONTROL_RESUME : PONG_CONTROL_PAUSE,
                0);
            if (network.is_host && s_game.input_ready &&
                network.local_ready && network.peer_ready &&
                s_game.physics.world.phase == PONG_PHASE_PAUSED) {
                start_countdown(timestamp_ms);
            }
        }
        s_game.reported_input_ready = s_game.input_ready;

        if (network.paired && s_game.input_ready &&
            ((edges | s_game.local_input.buttons) & PONG_BUTTON_CONFIRM) != 0 &&
            !network.local_ready) {
            const esp_err_t ready_ret = pong_network_set_ready(true);
            if (ready_ret == ESP_OK) {
                network.local_ready = true;
                post_feedback_cue(PONG_FEEDBACK_CUE_READY);
                ESP_LOGI(TAG, "Local player ready: role=%s",
                         network.local_role == PONG_ROLE_LEFT ?
                         "LEFT" : "RIGHT");
            } else {
                ESP_LOGW(TAG, "Set local ready failed: %s",
                         esp_err_to_name(ready_ret));
            }
        }
        if (network.paired && s_game.input_ready &&
            (edges & PONG_BUTTON_PAUSE) != 0) {
            if (network.is_host) {
                if (s_game.physics.world.phase == PONG_PHASE_PAUSED &&
                    network.local_ready && network.peer_ready) {
                    start_countdown(timestamp_ms);
                    post_feedback_cue(PONG_FEEDBACK_CUE_RESUMED);
                } else {
                    s_game.physics.world.phase = PONG_PHASE_PAUSED;
                    post_feedback_cue(PONG_FEEDBACK_CUE_PAUSED);
                }
            } else {
                const bool resume =
                    s_game.client_world.phase == PONG_PHASE_PAUSED;
                (void)pong_network_send_control(
                    resume ?
                    PONG_CONTROL_RESUME : PONG_CONTROL_PAUSE, 0);
                post_feedback_cue(resume ? PONG_FEEDBACK_CUE_RESUMED :
                                  PONG_FEEDBACK_CUE_PAUSED);
            }
        }
        if (network.paired && s_game.input_ready &&
            (edges & PONG_BUTTON_EMOTE) != 0) {
            const esp_err_t emote_ret = pong_network_send_control(
                PONG_CONTROL_EMOTE, 0);
            if (emote_ret == ESP_OK) {
                post_feedback_cue(PONG_FEEDBACK_CUE_EMOTE);
            }
        }
        if (network.paired) {
            if (network.is_host) {
                run_host(timestamp_ms, &network, edges, physics_steps);
            } else {
                run_client(timestamp_ms, &network);
            }
        } else {
            s_game.physics.world.phase = s_game.input_ready ?
                                         PONG_PHASE_LOBBY :
                                         PONG_PHASE_CALIBRATING;
        }
        publish_render(&network);
        const uint32_t delay_ms = (++step_index % 3U) == 0U ? 9U : 8U;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(delay_ms));
    }
    vTaskDelete(NULL);
}

esp_err_t pong_game_start(void)
{
    if (s_game.running) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_game, 0, sizeof(s_game));
    s_game.render_lock = xSemaphoreCreateMutex();
    if (s_game.render_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = pong_input_create(&s_game.input);
    if (err != ESP_OK) {
        pong_game_stop();
        return err;
    }
    err = pong_network_start();
    if (err != ESP_OK) {
        pong_game_stop();
        return err;
    }
    pong_network_status_t status = {0};
    ESP_ERROR_CHECK(pong_network_get_status(&status));
    pong_physics_init(&s_game.physics, status.boot_id ^ (uint32_t)status.device_id);

    err = pong_docking_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Magnetic layout unavailable: %s", esp_err_to_name(err));
    }
    err = pong_feedback_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Feedback unavailable: %s", esp_err_to_name(err));
    }

    s_game.running = true;
    if (xTaskCreate(game_task, "pong_game", GAME_TASK_STACK_SIZE, NULL,
                    GAME_TASK_PRIORITY, &s_game.task) != pdPASS) {
        s_game.running = false;
        pong_game_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void pong_game_stop(void)
{
    s_game.running = false;
    if (s_game.task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
        s_game.task = NULL;
    }
    pong_feedback_deinit();
    pong_docking_stop();
    (void)pong_network_stop();
    pong_input_destroy(s_game.input);
    s_game.input = NULL;
    if (s_game.render_lock != NULL) {
        vSemaphoreDelete(s_game.render_lock);
        s_game.render_lock = NULL;
    }
}

bool pong_game_get_render_snapshot(pong_render_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_game.render_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_game.render_lock, portMAX_DELAY);
    *snapshot = s_game.render;
    xSemaphoreGive(s_game.render_lock);
    return true;
}
