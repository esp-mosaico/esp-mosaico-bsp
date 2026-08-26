/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include "pong_physics.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define PADDLE_MAX_TILT        1.0f
#define BALL_DRAG_PER_STEP     0.9994f
#define SPIN_DRAG_PER_STEP     0.9970f
#define SPIN_LIFT              0.045f
#define PADDLE_VELOCITY_GAIN   0.32f
#define PADDLE_TILT_GAIN       185.0f
#define HIT_OFFSET_GAIN        330.0f
#define MAX_COLLISIONS         6
#define COLLISION_EPSILON      0.01f

static float clampf(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

float pong_physics_paddle_velocity(float axis_y)
{
    if (!isfinite(axis_y)) {
        return 0.0f;
    }
    const float axis = clampf(axis_y, -1.0f, 1.0f);
    const float magnitude = fabsf(axis);
    /*
     * Slightly boost the middle of the stick travel while preserving full
     * scale. This feels more immediate without sacrificing center control.
     */
    const float response = axis * (1.20f - 0.20f * magnitude);
    return response * PONG_PADDLE_MAX_SPEED;
}

static uint32_t prng_next(pong_physics_t *physics)
{
    uint32_t x = physics->prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    physics->prng_state = x;
    return x;
}

static void set_event(pong_world_t *world, pong_event_kind_t event)
{
    world->event = event;
    world->event_id++;
}

static void limit_ball_speed(pong_ball_t *ball)
{
    const float speed_sq = ball->velocity.x * ball->velocity.x +
                           ball->velocity.y * ball->velocity.y;
    const float max_sq = PONG_MAX_BALL_SPEED * PONG_MAX_BALL_SPEED;
    if (speed_sq > max_sq) {
        const float scale = PONG_MAX_BALL_SPEED / sqrtf(speed_sq);
        ball->velocity.x *= scale;
        ball->velocity.y *= scale;
    }
}

static void score_goal(pong_physics_t *physics, pong_role_t scorer)
{
    pong_world_t *world = &physics->world;
    if (world->score[scorer] < UINT8_MAX) {
        world->score[scorer]++;
    }
    world->serving_role = (uint8_t)(scorer == PONG_ROLE_LEFT ?
                                     PONG_ROLE_RIGHT : PONG_ROLE_LEFT);
    world->ball.position.x = PONG_WORLD_WIDTH * 0.5f;
    world->ball.position.y = PONG_WORLD_HEIGHT * 0.5f;
    world->ball.velocity.x = 0.0f;
    world->ball.velocity.y = 0.0f;
    world->ball.spin = 0.0f;
    world->phase = world->score[scorer] >= PONG_SCORE_TO_WIN ?
                   PONG_PHASE_MATCH_OVER : PONG_PHASE_ROUND_OVER;
    set_event(world, world->phase == PONG_PHASE_MATCH_OVER ?
                     PONG_EVENT_MATCH_WIN : PONG_EVENT_GOAL);
}

static void update_paddles(pong_physics_t *physics)
{
    const float half_height = PONG_PADDLE_HEIGHT * 0.5f;
    for (size_t i = 0; i < 2; ++i) {
        pong_paddle_t *paddle = &physics->world.paddles[i];
        const pong_input_t *input = &physics->inputs[i];
        const float requested_velocity =
            pong_physics_paddle_velocity(input->axis_y);
        paddle->tilt = clampf(input->axis_x, -1.0f, 1.0f) *
                       PADDLE_MAX_TILT;
        const float old_y = paddle->y;
        paddle->y = clampf(paddle->y +
                           requested_velocity * PONG_PHYSICS_STEP_SECONDS,
                           half_height, PONG_WORLD_HEIGHT - half_height);
        paddle->velocity = (paddle->y - old_y) / PONG_PHYSICS_STEP_SECONDS;
    }
}

static bool paddle_collision_time(const pong_ball_t *ball,
                                  const pong_paddle_t *paddle,
                                  pong_role_t role, float remaining,
                                  float *collision_time)
{
    const float center_x = role == PONG_ROLE_LEFT ?
                           PONG_PADDLE_LEFT_X : PONG_PADDLE_RIGHT_X;
    const float face_x = role == PONG_ROLE_LEFT ?
                         center_x + PONG_PADDLE_WIDTH * 0.5f +
                             PONG_BALL_RADIUS :
                         center_x - PONG_PADDLE_WIDTH * 0.5f -
                             PONG_BALL_RADIUS;
    if ((role == PONG_ROLE_LEFT && ball->velocity.x >= 0.0f) ||
        (role == PONG_ROLE_RIGHT && ball->velocity.x <= 0.0f)) {
        return false;
    }

    const float time = (face_x - ball->position.x) / ball->velocity.x;
    if (time < 0.0f || time > remaining) {
        return false;
    }
    const float y = ball->position.y + ball->velocity.y * time;
    const float reach = PONG_PADDLE_HEIGHT * 0.5f + PONG_BALL_RADIUS;
    if (fabsf(y - paddle->y) > reach) {
        return false;
    }
    *collision_time = time;
    return true;
}

static void bounce_from_paddle(pong_ball_t *ball,
                               const pong_paddle_t *paddle,
                               pong_role_t role)
{
    const float reach = PONG_PADDLE_HEIGHT * 0.5f + PONG_BALL_RADIUS;
    const float offset = clampf((ball->position.y - paddle->y) / reach,
                                -1.0f, 1.0f);
    const float incoming_speed = hypotf(ball->velocity.x, ball->velocity.y);
    float horizontal = fmaxf(incoming_speed * 0.82f, PONG_SERVE_SPEED);
    horizontal = fminf(horizontal * 1.025f, PONG_MAX_BALL_SPEED);
    ball->velocity.x = role == PONG_ROLE_LEFT ? horizontal : -horizontal;
    ball->velocity.y += offset * HIT_OFFSET_GAIN +
                        paddle->velocity * PADDLE_VELOCITY_GAIN +
                        paddle->tilt * PADDLE_TILT_GAIN;
    ball->spin = clampf(ball->spin + offset * 7.0f +
                        paddle->tilt * 4.0f +
                        paddle->velocity * 0.008f, -16.0f, 16.0f);
    limit_ball_speed(ball);
}

static void simulate_ball(pong_physics_t *physics)
{
    pong_world_t *world = &physics->world;
    pong_ball_t *ball = &world->ball;
    float remaining = PONG_PHYSICS_STEP_SECONDS;
    const float old_x = ball->position.x;

    ball->velocity.y += ball->spin * SPIN_LIFT;
    ball->velocity.x *= BALL_DRAG_PER_STEP;
    ball->velocity.y *= BALL_DRAG_PER_STEP;
    ball->spin *= SPIN_DRAG_PER_STEP;
    limit_ball_speed(ball);

    for (int collision = 0; collision < MAX_COLLISIONS && remaining > 0.0f;
         ++collision) {
        float hit_time = remaining;
        int hit = 0; /* 1 top/bottom, 2 left paddle, 3 right paddle */

        if (ball->velocity.y < 0.0f) {
            const float t = (PONG_BALL_RADIUS - ball->position.y) /
                            ball->velocity.y;
            if (t >= 0.0f && t < hit_time) {
                hit_time = t;
                hit = 1;
            }
        } else if (ball->velocity.y > 0.0f) {
            const float t = (PONG_WORLD_HEIGHT - PONG_BALL_RADIUS -
                             ball->position.y) / ball->velocity.y;
            if (t >= 0.0f && t < hit_time) {
                hit_time = t;
                hit = 1;
            }
        }

        float paddle_time;
        if (paddle_collision_time(ball, &world->paddles[PONG_ROLE_LEFT],
                                  PONG_ROLE_LEFT, hit_time, &paddle_time)) {
            hit_time = paddle_time;
            hit = 2;
        }
        if (paddle_collision_time(ball, &world->paddles[PONG_ROLE_RIGHT],
                                  PONG_ROLE_RIGHT, hit_time, &paddle_time)) {
            hit_time = paddle_time;
            hit = 3;
        }

        ball->position.x += ball->velocity.x * hit_time;
        ball->position.y += ball->velocity.y * hit_time;
        remaining -= hit_time;
        if (hit == 0) {
            break;
        }
        if (hit == 1) {
            ball->velocity.y = -ball->velocity.y;
            ball->spin = -ball->spin * 0.85f;
            ball->position.y = clampf(ball->position.y,
                                      PONG_BALL_RADIUS + COLLISION_EPSILON,
                                      PONG_WORLD_HEIGHT - PONG_BALL_RADIUS -
                                          COLLISION_EPSILON);
            set_event(world, PONG_EVENT_WALL_HIT);
        } else {
            const pong_role_t role = hit == 2 ?
                                     PONG_ROLE_LEFT : PONG_ROLE_RIGHT;
            bounce_from_paddle(ball, &world->paddles[role], role);
            ball->position.x += role == PONG_ROLE_LEFT ?
                                COLLISION_EPSILON : -COLLISION_EPSILON;
            set_event(world, PONG_EVENT_PADDLE_HIT);
        }
        remaining = fmaxf(0.0f, remaining - 0.000001f);
    }

    if (ball->position.x < -PONG_BALL_RADIUS) {
        score_goal(physics, PONG_ROLE_RIGHT);
    } else if (ball->position.x > PONG_WORLD_WIDTH + PONG_BALL_RADIUS) {
        score_goal(physics, PONG_ROLE_LEFT);
    } else if ((old_x < PONG_VIEWPORT_WIDTH &&
                ball->position.x >= PONG_VIEWPORT_WIDTH) ||
               (old_x >= PONG_VIEWPORT_WIDTH &&
                ball->position.x < PONG_VIEWPORT_WIDTH)) {
        set_event(world, PONG_EVENT_SEAM_CROSS);
    }
}

void pong_physics_init(pong_physics_t *physics, uint32_t host_seed)
{
    if (physics == NULL) {
        return;
    }
    memset(physics, 0, sizeof(*physics));
    physics->prng_state = host_seed == 0U ? 0x6d2b79f5U : host_seed;
    physics->world.phase = PONG_PHASE_LOBBY;
    physics->world.ball.position.x = PONG_WORLD_WIDTH * 0.5f;
    physics->world.ball.position.y = PONG_WORLD_HEIGHT * 0.5f;
    physics->world.paddles[PONG_ROLE_LEFT].y = PONG_WORLD_HEIGHT * 0.5f;
    physics->world.paddles[PONG_ROLE_RIGHT].y = PONG_WORLD_HEIGHT * 0.5f;
    physics->world.serving_role = PONG_ROLE_LEFT;
}

void pong_physics_start_round(pong_physics_t *physics,
                              pong_role_t serving_role)
{
    if (physics == NULL ||
        (serving_role != PONG_ROLE_LEFT &&
         serving_role != PONG_ROLE_RIGHT)) {
        return;
    }
    pong_world_t *world = &physics->world;
    const uint32_t random = prng_next(physics);
    const float vertical = ((float)((random >> 8) & 0xffffU) / 65535.0f -
                            0.5f) * 0.9f * PONG_SERVE_SPEED;
    world->ball.position.x = PONG_WORLD_WIDTH * 0.5f;
    world->ball.position.y = PONG_WORLD_HEIGHT * 0.5f;
    world->ball.velocity.x = serving_role == PONG_ROLE_LEFT ?
                             PONG_SERVE_SPEED : -PONG_SERVE_SPEED;
    world->ball.velocity.y = vertical;
    world->ball.spin = 0.0f;
    world->serving_role = (uint8_t)serving_role;
    world->countdown_ms = 0;
    world->phase = PONG_PHASE_PLAYING;
    set_event(world, PONG_EVENT_SERVE);
}

void pong_physics_set_input(pong_physics_t *physics, pong_role_t role,
                            const pong_input_t *input)
{
    if (physics == NULL || input == NULL ||
        (role != PONG_ROLE_LEFT && role != PONG_ROLE_RIGHT)) {
        return;
    }
    physics->inputs[role] = *input;
    if (!isfinite(physics->inputs[role].axis_x)) {
        physics->inputs[role].axis_x = 0.0f;
    }
    if (!isfinite(physics->inputs[role].axis_y)) {
        physics->inputs[role].axis_y = 0.0f;
    }
    physics->inputs[role].axis_x =
        clampf(physics->inputs[role].axis_x, -1.0f, 1.0f);
    physics->inputs[role].axis_y =
        clampf(physics->inputs[role].axis_y, -1.0f, 1.0f);
}

void pong_physics_step(pong_physics_t *physics)
{
    if (physics == NULL) {
        return;
    }
    physics->world.tick++;
    /*
     * Keep the last event attached to its event_id until a newer event occurs.
     * Snapshots run at 30 Hz while physics runs at 120 Hz, so clearing it after
     * one tick would make most collision feedback invisible to the client.
     */
    update_paddles(physics);
    if (physics->world.phase == PONG_PHASE_PLAYING) {
        simulate_ball(physics);
    }
}

void pong_physics_predict_ball(const pong_world_t *world, uint32_t steps,
                               pong_ball_t *predicted_ball)
{
    if (world == NULL || predicted_ball == NULL) {
        return;
    }

    pong_physics_t prediction = {
        .world = *world,
    };
    for (uint32_t step = 0; step < steps &&
         prediction.world.phase == PONG_PHASE_PLAYING; ++step) {
        simulate_ball(&prediction);
    }
    *predicted_ball = prediction.world.ball;
}

const pong_world_t *pong_physics_world(const pong_physics_t *physics)
{
    return physics == NULL ? NULL : &physics->world;
}
