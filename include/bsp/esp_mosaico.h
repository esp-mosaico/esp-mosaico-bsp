/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ESP BSP: ESP-Mosaico
 */

#pragma once

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "bsp/audio.h"
#include "bsp/battery.h"
#include "bsp/button.h"
#include "bsp/display.h"
#include "bsp/imu.h"
#include "bsp/led.h"
#include "bsp/magnetometer.h"
#include "bsp/motor.h"
#include "bsp/nand_flash.h"
#include "bsp/power.h"
#include "bsp/subboard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_BOARD_ESP_MOSAICO

#define BSP_CAPS_DISPLAY          1
#define BSP_CAPS_TOUCH            1
#define BSP_CAPS_BUTTONS          1
#define BSP_CAPS_AUDIO            1
#define BSP_CAPS_AUDIO_SPEAKER    1
#define BSP_CAPS_AUDIO_MIC        1
#define BSP_CAPS_NAND_FLASH       1
#define BSP_CAPS_LED              1
#define BSP_CAPS_MOTOR            1
#define BSP_CAPS_IMU              1
#define BSP_CAPS_MAGNETOMETER     1
#define BSP_CAPS_BATTERY          1
#define BSP_CAPS_USB_OTG          1
#define BSP_CAPS_SDCARD           0
#define BSP_CAPS_CAMERA           0
#define BSP_CAPS_SUBBOARDS        1

/* Shared I2C0 bus */
#define BSP_I2C_PORT              I2C_NUM_0
#define BSP_I2C_SDA               GPIO_NUM_0
#define BSP_I2C_SCL               GPIO_NUM_1

/* Shared sensor interrupt, diode-ORed on the board */
#define BSP_SENSOR_INT            GPIO_NUM_2

/* CO5300 QSPI LCD */
#define BSP_LCD_SPI_HOST          SPI2_HOST
#define BSP_LCD_RST               GPIO_NUM_42
#define BSP_LCD_TE                GPIO_NUM_43
#define BSP_LCD_CS                GPIO_NUM_50
#define BSP_LCD_SCL               GPIO_NUM_44
#define BSP_LCD_DATA0             GPIO_NUM_36
#define BSP_LCD_DATA1             GPIO_NUM_51
#define BSP_LCD_DATA2             GPIO_NUM_35
#define BSP_LCD_DATA3             GPIO_NUM_9
#define BSP_LCD_EN                GPIO_NUM_NC
#define BSP_LCD_BACKLIGHT         GPIO_NUM_NC

/* CO5300 touch */
#define BSP_LCD_TOUCH_I2C_SDA     BSP_I2C_SDA
#define BSP_LCD_TOUCH_I2C_SCL     BSP_I2C_SCL
#define BSP_LCD_TOUCH_INT         GPIO_NUM_6
#define BSP_LCD_TOUCH_RST         GPIO_NUM_NC
#define BSP_LCD_TOUCH_I2C_TIMEOUT_MS 100

/* Audio */
#define BSP_AUDIO_I2S_MCLK        GPIO_NUM_54
#define BSP_AUDIO_I2S_SCLK        GPIO_NUM_37
#define BSP_AUDIO_I2S_LRCLK       GPIO_NUM_49
#define BSP_AUDIO_I2S_SDOUT       GPIO_NUM_52
#define BSP_AUDIO_I2S_DSIN        GPIO_NUM_40
#define BSP_AUDIO_PA_CTRL         GPIO_NUM_45
/* esp_codec_dev expects the 8-bit form of the schematic's 7-bit address 0x19. */
#define BSP_AUDIO_CODEC_I2C_ADDR  (0x19U << 1)

/* Board power rails: VCC_PW is active-low; CODEC_PW is active-high */
#define BSP_POWER_VCC_3V3_CTRL         GPIO_NUM_60
#define BSP_POWER_VCC_3V3_ON_LEVEL     0
#define BSP_POWER_VCC_3V3_OFF_LEVEL    1
#define BSP_POWER_CODEC_3V3_CTRL       GPIO_NUM_56
#define BSP_POWER_CODEC_3V3_ON_LEVEL   1
#define BSP_POWER_CODEC_3V3_OFF_LEVEL  0
#define BSP_POWER_SWITCH_GPIO          GPIO_NUM_57
#define BSP_POWER_SWITCH_ASSERT_LEVEL  0
#define BSP_POWER_SWITCH_RELEASE_LEVEL 1

/* Application buttons, LED and motor */
#define BSP_BUTTON_AI_GPIO        GPIO_NUM_7
#define BSP_BUTTON_BOOT_GPIO      GPIO_NUM_61
#define BSP_BUTTON_ACTIVE_LEVEL   0
#define BSP_LED_STATUS_GPIO       GPIO_NUM_3
#define BSP_LED_ON_LEVEL          0
#define BSP_LED_OFF_LEVEL         1
#define BSP_MOTOR_GPIO            GPIO_NUM_8
#define BSP_MOTOR_ON_LEVEL        1
#define BSP_MOTOR_OFF_LEVEL       0

/* SPI NAND: schematic NAND_* nets are routed through the ESP32-S31 SD_* pins. */
#define BSP_NAND_SPI_HOST         SPI3_HOST  /* SPI2_HOST is reserved for the CO5300 LCD */
#define BSP_NAND_CLK              GPIO_NUM_20  /* NAND_CLK -> SD_D0 */
#define BSP_NAND_D                GPIO_NUM_21  /* NAND_D   -> SD_D1 / SIO0 */
#define BSP_NAND_Q                GPIO_NUM_22  /* NAND_Q   -> SD_D2 / SIO1 */
#define BSP_NAND_CS               GPIO_NUM_23  /* NAND_CS  -> SD_D3 */
#define BSP_NAND_HOLD             GPIO_NUM_24  /* NAND_HD  -> SD_CLK / SIO3 */
#define BSP_NAND_WP               GPIO_NUM_25  /* NAND_WP  -> SD_CMD / SIO2 */

/* Sensors */
#define BSP_IMU_I2C_ADDR          0x69
#define BSP_IMU_INT               BSP_SENSOR_INT
/* Two of the four magnetometer footprints are placed. */
#define BSP_BMM150_NUM            2
#define BSP_BMM150_ADDR_0         0x11
#define BSP_BMM150_ADDR_1         0x12
#define BSP_BMM150_INT            BSP_SENSOR_INT

/* BQ27220 battery fuel gauge on the shared I2C bus */
#define BSP_BATTERY_I2C_ADDR      0x55
#define BSP_BATTERY_I2C_SPEED_HZ  400000

#ifdef __cplusplus
}
#endif
