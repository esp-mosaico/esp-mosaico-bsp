/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>

#include "pong_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PONG_PHYSICS_HZ              120U
#define PONG_PHYSICS_STEP_SECONDS    (1.0f / (float)PONG_PHYSICS_HZ)
#define PONG_SERVE_SPEED             430.0f
#define PONG_MAX_BALL_SPEED          980.0f
#define PONG_PADDLE_MAX_SPEED        600.0f

typedef struct {
    pong_world_t world;
    pong_input_t inputs[2];
    uint32_t prng_state;
} pong_physics_t;

/**
 * Initialize authoritative simulation state.
 *
 * The seed belongs to the host. Followers must consume snapshots and must not
 * call pong_physics_start_round() to make independent serve decisions.
 */
void pong_physics_init(pong_physics_t *physics, uint32_t host_seed);

/**
 * Begin a round for the supplied serving player.
 *
 * Serve angle and vertical direction are selected only from the host PRNG.
 */
void pong_physics_start_round(pong_physics_t *physics,
                              pong_role_t serving_role);

/**
 * Store the latest input for one paddle.
 *
 * axis_y controls paddle speed, axis_x controls tilt, and both are clamped.
 */
void pong_physics_set_input(pong_physics_t *physics, pong_role_t role,
                            const pong_input_t *input);

/**
 * Convert normalized joystick Y into the tuned paddle velocity.
 */
float pong_physics_paddle_velocity(float axis_y);

/**
 * Advance exactly one fixed 1/120 second authoritative tick.
 */
void pong_physics_step(pong_physics_t *physics);

/**
 * Predict only the ball for render-side extrapolation.
 *
 * Paddle positions, velocity and tilt are taken from @p world, while scoring
 * and events remain private to the temporary prediction state.
 */
void pong_physics_predict_ball(const pong_world_t *world, uint32_t steps,
                               pong_ball_t *predicted_ball);

/**
 * Return the current authoritative world snapshot.
 */
const pong_world_t *pong_physics_world(const pong_physics_t *physics);

#ifdef __cplusplus
}
#endif
