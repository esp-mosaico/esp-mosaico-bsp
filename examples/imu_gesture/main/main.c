/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief Simple BMI270 gestures: shake / tilt / flip.
 */

#include <math.h>
#include <stdbool.h>

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu_gesture";

#define SAMPLE_PERIOD_MS     40
#define STARTUP_DELAY_MS     80
#define COOLDOWN_MS          600
#define SHAKE_DELTA_G        1.6f
#define TILT_XY_G            0.75f
#define FLIP_Z_G            -0.70f
#define MOTOR_PULSE_MS       80

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_SHAKE,
    GESTURE_TILT,
    GESTURE_FLIP,
} gesture_t;

static const char *gesture_name(gesture_t g)
{
    switch (g) {
    case GESTURE_SHAKE:
        return "shake";
    case GESTURE_TILT:
        return "tilt";
    case GESTURE_FLIP:
        return "flip";
    default:
        return "none";
    }
}

static void feedback(void)
{
    (void)bsp_led_set(true);
    (void)bsp_motor_set(true);
    vTaskDelay(pdMS_TO_TICKS(MOTOR_PULSE_MS));
    (void)bsp_motor_set(false);
    (void)bsp_led_set(false);
}

static gesture_t classify(float ax, float ay, float az, float prev_mag, float cur_mag)
{
    if (fabsf(cur_mag - prev_mag) >= SHAKE_DELTA_G || cur_mag >= (1.0f + SHAKE_DELTA_G)) {
        return GESTURE_SHAKE;
    }
    if (az <= FLIP_Z_G) {
        return GESTURE_FLIP;
    }
    if ((fabsf(ax) >= TILT_XY_G || fabsf(ay) >= TILT_XY_G) && fabsf(az) < 0.85f) {
        return GESTURE_TILT;
    }
    return GESTURE_NONE;
}

void app_main(void)
{
    const bsp_imu_config_t config = BSP_IMU_CONFIG_DEFAULT();

    ESP_LOGI(TAG, "BMI270 gesture demo");
    ESP_ERROR_CHECK(bsp_led_init());
    ESP_ERROR_CHECK(bsp_motor_init());
    ESP_ERROR_CHECK(bsp_imu_init());
    ESP_ERROR_CHECK(bsp_imu_start(&config));
    vTaskDelay(pdMS_TO_TICKS(STARTUP_DELAY_MS));

    float prev_mag = 1.0f;
    gesture_t last = GESTURE_NONE;
    TickType_t cool_until = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        float ax = 0, ay = 0, az = 0;
        if (bsp_imu_get_accel(&ax, &ay, &az) != ESP_OK) {
            xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }

        const float mag = sqrtf(ax * ax + ay * ay + az * az);
        const gesture_t g = classify(ax, ay, az, prev_mag, mag);
        prev_mag = mag;

        const TickType_t now = xTaskGetTickCount();
        if (g != GESTURE_NONE && g != last && now >= cool_until) {
            ESP_LOGI(TAG, "gesture=%s accel[g]=%+.2f %+.2f %+.2f |mag|=%.2f",
                     gesture_name(g), ax, ay, az, mag);
            feedback();
            last = g;
            cool_until = now + pdMS_TO_TICKS(COOLDOWN_MS);
        } else if (g == GESTURE_NONE) {
            last = GESTURE_NONE;
        }

        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}
