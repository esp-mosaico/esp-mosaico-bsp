/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "eeprom_program.h"

#include <stdio.h>
#include <string.h>

#include "bsp/subboard.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EEPROM_PAGE_SIZE 8U
#define EEPROM_WRITE_CYCLE_MS 5

static const char *TAG = "eeprom_program";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_devices[2];
static const eeprom_board_profile_t s_profiles[] = {
    {"Camera Board", MOSAICO_BOARD_TYPE_CAMERA, 1, "CameraBoard"},
    {"Button LED Board", 0x14, 1, "ButtonLedBoard"},
    {"Sensor Board", MOSAICO_BOARD_TYPE_SENSOR, 1, "SensorBoard"},
    {"TOF Board", MOSAICO_BOARD_TYPE_TOF, 1, "TofBoard"},
    {"Matrix LED Board", MOSAICO_BOARD_TYPE_MATRIX_LED, 1, "MatrixLedBoard"},
    {"Thermal Board", MOSAICO_BOARD_TYPE_THERMAL, 1, "ThermalBoard"},
    {"Relay Board", MOSAICO_BOARD_TYPE_RELAY, 1, "RelayBoard"},
    {"IO Test Board", 0x15, 1, "IoTestBoard"},
    {"Interaction Board", MOSAICO_BOARD_TYPE_INTERACT, 1, "InteractionBoard"},
};

size_t eeprom_program_profile_count(void)
{
    return sizeof(s_profiles) / sizeof(s_profiles[0]);
}

const eeprom_board_profile_t *eeprom_program_get_profile(size_t index)
{
    return index < eeprom_program_profile_count() ? &s_profiles[index] : NULL;
}

static void write_le16(uint8_t *dest, uint16_t value)
{
    dest[0] = value & 0xFF;
    dest[1] = value >> 8;
}

static void update_crc(uint8_t *image, size_t start, size_t end)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = start; i < end; ++i) {
        crc ^= image[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
        }
    }
    write_le16(image + end, crc);
}

static void build_image(const eeprom_board_profile_t *profile, uint8_t *image)
{
    /* Encode the V1 wire format explicitly; the manager's struct is not packed. */
    memset(image, 0, MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE);
    memcpy(image, MOSAICO_MODULE_MGR_EEPROM_MAGIC, MOSAICO_MODULE_MGR_EEPROM_MAGIC_LEN);
    image[0x03] = profile->board_type;
    write_le16(image + 0x04, profile->board_id);
    write_le16(image + 0x06, 0x0102);
    write_le16(image + 0x08, 0x0100);
    write_le16(image + 0x0A, 1);
    image[0x10] = 1;  /* Default serial number. */
    snprintf((char *)image + 0x14, 32, "%s", profile->board_name);
    write_le16(image + 0x40, 1);
    update_crc(image, 0, 0x34);
    update_crc(image, 0x36, 0x3E);
    update_crc(image, 0x40, 0x84);
}

esp_err_t eeprom_program_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_bus || s_devices[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Register before touch starts: v1.0 shares this bus with the touch driver. */
    for (size_t i = 0; i < 2; ++i) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = i == 0 ? BSP_SUBBOARD_EEPROM_ADDR_LEFT : BSP_SUBBOARD_EEPROM_ADDR_RIGHT,
            .scl_speed_hz = 100000,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &config, &s_devices[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Register EEPROM 0x%02X failed: %s", config.device_address, esp_err_to_name(err));
            if (i > 0) {
                esp_err_t cleanup_err = i2c_master_bus_rm_device(s_devices[0]);
                if (cleanup_err == ESP_OK) {
                    s_devices[0] = NULL;
                } else {
                    ESP_LOGE(TAG, "EEPROM init cleanup failed: %s", esp_err_to_name(cleanup_err));
                }
            }
            return err;
        }
    }
    s_bus = bus;
    return ESP_OK;
}

esp_err_t eeprom_program_run(size_t profile_index, eeprom_program_progress_cb_t progress_cb,
                            eeprom_program_result_t *result)
{
    const eeprom_board_profile_t *profile = eeprom_program_get_profile(profile_index);
    if (!profile || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(result, 0, sizeof(*result));
    uint8_t addr = BSP_SUBBOARD_EEPROM_ADDR_LEFT;
    result->slot = MOSAICO_MODULE_MGR_SLOT_LEFT;
    esp_err_t err = i2c_master_probe(s_bus, addr, 100);
    /* Only an absent left EEPROM permits trying the right address. */
    if (err == ESP_ERR_NOT_FOUND) {
        addr = BSP_SUBBOARD_EEPROM_ADDR_RIGHT;
        result->slot = MOSAICO_MODULE_MGR_SLOT_RIGHT;
        err = i2c_master_probe(s_bus, addr, 100);
    }
    if (err != ESP_OK) {
        snprintf(result->message, sizeof(result->message), "Probe failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Probe 0x%02X failed: %s", addr, esp_err_to_name(err));
        return err;
    }

    uint8_t image[MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE];
    build_image(profile, image);
    i2c_master_dev_handle_t device = s_devices[result->slot];

    ESP_LOGI(TAG, "Writing %s to 0x%02X, module hw=v1.2", profile->label, addr);
    for (size_t offset = 0; offset < sizeof(image); offset += EEPROM_PAGE_SIZE) {
        size_t count = sizeof(image) - offset;
        if (count > EEPROM_PAGE_SIZE) {
            count = EEPROM_PAGE_SIZE;
        }
        uint8_t page[1 + EEPROM_PAGE_SIZE];
        page[0] = offset;
        memcpy(page + 1, image + offset, count);
        err = i2c_master_transmit(device, page, count + 1, 100);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write 0x%02X offset 0x%02X failed: %s", addr, (unsigned)offset, esp_err_to_name(err));
            break;
        }
        /* Round up so the EEPROM always gets its full write-cycle time. */
        vTaskDelay(pdMS_TO_TICKS(EEPROM_WRITE_CYCLE_MS) + 1);
        if (progress_cb) {
            progress_cb((offset + count) * 90 / sizeof(image), "Writing EEPROM...");
        }
    }
    if (err == ESP_OK) {
        uint8_t readback[sizeof(image)];
        const uint8_t offset = 0;
        err = i2c_master_transmit_receive(device, &offset, 1, readback, sizeof(readback), 100);
        if (err == ESP_OK && memcmp(image, readback, sizeof(image)) != 0) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Verify 0x%02X failed: %s", addr, esp_err_to_name(err));
        }
    }
    result->success = err == ESP_OK;
    snprintf(result->message, sizeof(result->message), "%s (0x%02X)\n%s", profile->label, addr,
             result->success ? "Write and verify OK" : esp_err_to_name(err));
    if (result->success) {
        ESP_LOGI(TAG, "Write verify SUCCESS at 0x%02X type=0x%02X name=%s", addr, profile->board_type, profile->board_name);
    }
    if (progress_cb) {
        progress_cb(100, result->success ? "Write succeeded" : "Write failed");
    }
    return err;
}
