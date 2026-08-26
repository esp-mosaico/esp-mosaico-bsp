/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>

#include "pong_physics.h"
#include "unity.h"

TEST_CASE("pong swept collision prevents paddle tunneling", "[pong][physics]")
{
    pong_physics_t physics;
    pong_physics_init(&physics, 1);
    pong_physics_start_round(&physics, PONG_ROLE_LEFT);
    physics.world.ball.position.x = 55.0f;
    physics.world.ball.position.y = physics.world.paddles[PONG_ROLE_LEFT].y;
    physics.world.ball.velocity.x = -5000.0f;
    physics.world.ball.velocity.y = 0.0f;

    pong_physics_step(&physics);

    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, physics.world.ball.velocity.x);
    TEST_ASSERT_EQUAL(PONG_EVENT_PADDLE_HIT, physics.world.event);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(52.0f, physics.world.ball.position.x);
}

TEST_CASE("pong render prediction also prevents paddle tunneling",
          "[pong][physics]")
{
    pong_physics_t physics;
    pong_ball_t predicted;
    pong_physics_init(&physics, 7);
    pong_physics_start_round(&physics, PONG_ROLE_LEFT);
    physics.world.ball.position.x = 90.0f;
    physics.world.ball.position.y = physics.world.paddles[PONG_ROLE_LEFT].y;
    physics.world.ball.velocity.x = -980.0f;
    physics.world.ball.velocity.y = 0.0f;

    pong_physics_predict_ball(&physics.world, 8, &predicted);

    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, predicted.velocity.x);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(
        PONG_PADDLE_LEFT_X + PONG_PADDLE_WIDTH * 0.5f + PONG_BALL_RADIUS,
        predicted.position.x);
}

TEST_CASE("pong ball reflects from horizontal wall", "[pong][physics]")
{
    pong_physics_t physics;
    pong_physics_init(&physics, 2);
    pong_physics_start_round(&physics, PONG_ROLE_RIGHT);
    physics.world.ball.position.y = PONG_BALL_RADIUS + 0.5f;
    physics.world.ball.velocity.x = 0.0f;
    physics.world.ball.velocity.y = -600.0f;

    pong_physics_step(&physics);

    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, physics.world.ball.velocity.y);
    TEST_ASSERT_EQUAL(PONG_EVENT_WALL_HIT, physics.world.event);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(PONG_BALL_RADIUS,
                                       physics.world.ball.position.y);
}

TEST_CASE("pong paddle motion tilt and offset shape return", "[pong][physics]")
{
    pong_physics_t physics;
    pong_input_t input = {
        .axis_x = 0.75f,
        .axis_y = 1.0f,
    };
    pong_physics_init(&physics, 3);
    pong_physics_set_input(&physics, PONG_ROLE_LEFT, &input);
    pong_physics_start_round(&physics, PONG_ROLE_LEFT);
    physics.world.ball.position.x = 55.0f;
    physics.world.ball.position.y =
        physics.world.paddles[PONG_ROLE_LEFT].y + 30.0f;
    physics.world.ball.velocity.x = -800.0f;
    physics.world.ball.velocity.y = 0.0f;

    pong_physics_step(&physics);

    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, physics.world.ball.velocity.x);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, physics.world.ball.velocity.y);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, physics.world.ball.spin);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.75f,
                             physics.world.paddles[PONG_ROLE_LEFT].tilt);
}

TEST_CASE("pong goal increments score and ends round", "[pong][physics]")
{
    pong_physics_t physics;
    pong_physics_init(&physics, 4);
    pong_physics_start_round(&physics, PONG_ROLE_LEFT);
    physics.world.ball.position.x = -PONG_BALL_RADIUS + 0.1f;
    physics.world.ball.position.y = 40.0f;
    physics.world.ball.velocity.x = -500.0f;
    physics.world.ball.velocity.y = 0.0f;

    pong_physics_step(&physics);

    TEST_ASSERT_EQUAL_UINT8(1, physics.world.score[PONG_ROLE_RIGHT]);
    TEST_ASSERT_EQUAL(PONG_PHASE_ROUND_OVER, physics.world.phase);
    TEST_ASSERT_EQUAL(PONG_EVENT_GOAL, physics.world.event);
    TEST_ASSERT_EQUAL(PONG_ROLE_LEFT, physics.world.serving_role);
}

TEST_CASE("pong host simulation is deterministic for equal seeds",
          "[pong][physics]")
{
    pong_physics_t first;
    pong_physics_t second;
    pong_input_t left = {
        .axis_x = -0.3f,
        .axis_y = 0.4f,
        .sequence = 9,
    };
    pong_physics_init(&first, 0x12345678);
    pong_physics_init(&second, 0x12345678);
    pong_physics_set_input(&first, PONG_ROLE_LEFT, &left);
    pong_physics_set_input(&second, PONG_ROLE_LEFT, &left);
    pong_physics_start_round(&first, PONG_ROLE_RIGHT);
    pong_physics_start_round(&second, PONG_ROLE_RIGHT);

    for (int i = 0; i < 600; ++i) {
        pong_physics_step(&first);
        pong_physics_step(&second);
    }

    TEST_ASSERT_EQUAL_MEMORY(&first.world, &second.world,
                             sizeof(first.world));
    TEST_ASSERT_EQUAL_UINT32(first.prng_state, second.prng_state);
}

TEST_CASE("pong corner collision stays inside court", "[pong][physics]")
{
    pong_physics_t physics;
    pong_physics_init(&physics, 5);
    pong_physics_start_round(&physics, PONG_ROLE_LEFT);
    physics.world.ball.position =
        (pong_vec2_t) { 56.0f, PONG_BALL_RADIUS + 1.0f };
    physics.world.paddles[PONG_ROLE_LEFT].y = PONG_PADDLE_HEIGHT * 0.5f;
    physics.world.ball.velocity = (pong_vec2_t) { -900.0f, -500.0f };

    for (int i = 0; i < 4; ++i) {
        pong_physics_step(&physics);
    }

    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(PONG_BALL_RADIUS,
                                       physics.world.ball.position.y);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(PONG_WORLD_HEIGHT - PONG_BALL_RADIUS,
                                    physics.world.ball.position.y);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, physics.world.ball.velocity.x);
}

TEST_CASE("pong spin curves the flight path", "[pong][physics]")
{
    pong_physics_t plain;
    pong_physics_t spinning;
    pong_physics_init(&plain, 6);
    pong_physics_init(&spinning, 6);
    pong_physics_start_round(&plain, PONG_ROLE_LEFT);
    pong_physics_start_round(&spinning, PONG_ROLE_LEFT);
    plain.world.ball.velocity = (pong_vec2_t) { 500.0f, 0.0f };
    spinning.world.ball.velocity = plain.world.ball.velocity;
    spinning.world.ball.spin = 12.0f;

    for (int i = 0; i < 120; ++i) {
        pong_physics_step(&plain);
        pong_physics_step(&spinning);
    }

    TEST_ASSERT_FLOAT_NOT_WITHIN(0.1f, plain.world.ball.position.y,
                                 spinning.world.ball.position.y);
    TEST_ASSERT_EQUAL_UINT32(120, plain.world.tick);
    TEST_ASSERT_EQUAL_UINT32(plain.world.tick, spinning.world.tick);
}

TEST_CASE("pong paddle response is fast and symmetric", "[pong][physics]")
{
    TEST_ASSERT_FLOAT_WITHIN(0.1f, PONG_PADDLE_MAX_SPEED,
                             pong_physics_paddle_velocity(1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, -PONG_PADDLE_MAX_SPEED,
                             pong_physics_paddle_velocity(-1.0f));
    TEST_ASSERT_GREATER_THAN_FLOAT(
        PONG_PADDLE_MAX_SPEED * 0.5f,
        pong_physics_paddle_velocity(0.5f));
    TEST_ASSERT_FLOAT_WITHIN(
        0.1f, pong_physics_paddle_velocity(0.5f),
        -pong_physics_paddle_velocity(-0.5f));
}
