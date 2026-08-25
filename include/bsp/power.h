/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_power_init(void);

/**
 * @brief Switch the VCC_3V3 rail on GPIO60 on or off
 *
 * Turning the rail on sweeps the enable pin with a PWM duty cycle and blocks for
 * CONFIG_BSP_VCC_3V3_RAMP_MS; turning it off drives the pin in one step. Calls
 * that match the current state return without repeating the ramp.
 */
esp_err_t bsp_power_set_vcc_3v3(bool on);
esp_err_t bsp_power_set_codec_3v3(bool on);

/**
 * @brief Assert or release the board shutdown signal on GPIO57
 *
 * GPIO57 is always configured as an open-drain output. Passing true actively
 * pulls the signal low and requests board shutdown. Passing false writes the
 * inactive high level to the open-drain output, which releases the pin to high
 * impedance; the BSP never actively drives this signal high.
 */
esp_err_t bsp_power_set_shutdown(bool shutdown);

/**
 * @brief Prepare peripherals for the lowest practical sleep current
 *
 * Best-effort sequence used before MCU deep sleep:
 * 1. CO5300 Display Off + Sleep In + Deep Standby (if display was initialized)
 * 2. LCD CS (GPIO50) released to high-impedance
 * 3. SPI NAND sync + standby (if NAND was initialized)
 *
 * Missing / uninitialized peripherals are skipped. Failures are logged and
 * returned; callers may still choose to enter deep sleep.
 */
esp_err_t bsp_power_prepare_sleep(void);

/**
 * @brief Enter deep sleep while retaining the board power-control GPIO states
 *
 * Calls `bsp_power_prepare_sleep()` first, then holds GPIO60/56/57 and starts
 * deep sleep. Wake sources, if required, must be configured by the caller
 * before invoking this function. If no wake source is configured, reset or a
 * power cycle is required. This function does not return.
 */
void bsp_power_enter_deep_sleep(void) __attribute__((noreturn));

esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

#ifdef __cplusplus
}
#endif
