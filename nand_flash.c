/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp32_s31_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "S31-Mosaico-NAND";
static spi_device_handle_t s_spi_device;
static spi_nand_flash_device_t *s_flash;
static bool s_bus_initialized;
static bool s_power_save;

static esp_err_t release_resources(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_flash) {
        esp_err_t ret = spi_nand_flash_deinit_device(s_flash);
        if (first_error == ESP_OK) {
            first_error = ret;
        }
        s_flash = NULL;
    }
    if (s_spi_device) {
        esp_err_t ret = spi_bus_remove_device(s_spi_device);
        if (first_error == ESP_OK) {
            first_error = ret;
        }
        s_spi_device = NULL;
    }
    if (s_bus_initialized) {
        esp_err_t ret = spi_bus_free(BSP_NAND_SPI_HOST);
        if (first_error == ESP_OK) {
            first_error = ret;
        }
        s_bus_initialized = false;
    }
    return first_error;
}

static esp_err_t configure_control_pin(gpio_num_t pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure NAND control GPIO%d failed", pin);
    return gpio_set_level(pin, 1);
}

esp_err_t bsp_nand_flash_init(const bsp_nand_flash_config_t *config, spi_nand_flash_device_t **out_handle)
{
    ESP_RETURN_ON_FALSE(!s_flash, ESP_ERR_INVALID_STATE, TAG, "SPI NAND is already initialized");
    bsp_nand_flash_config_t active = config ? *config : (bsp_nand_flash_config_t)BSP_NAND_FLASH_DEFAULT_CONFIG();
    if (!active.clock_speed_hz) active.clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ;
    if (!active.queue_size) active.queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE;
    if (!active.max_transfer_sz) active.max_transfer_sz = BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ;
    const bool quad = active.io_mode == SPI_NAND_IO_MODE_QOUT || active.io_mode == SPI_NAND_IO_MODE_QIO;
    if (!quad) {
        ESP_RETURN_ON_ERROR(configure_control_pin(BSP_NAND_HOLD), TAG, "release NAND HOLD failed");
        ESP_RETURN_ON_ERROR(configure_control_pin(BSP_NAND_WP), TAG, "release NAND WP failed");
    }
    const spi_bus_config_t bus_config = {
        .mosi_io_num = BSP_NAND_D,
        .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK,
        .quadhd_io_num = quad ? BSP_NAND_HOLD : GPIO_NUM_NC,
        .quadwp_io_num = quad ? BSP_NAND_WP : GPIO_NUM_NC,
        .max_transfer_sz = active.max_transfer_sz,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_NAND_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG,
                        "initialize SPI3 NAND bus failed");
    s_bus_initialized = true;
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = active.clock_speed_hz,
        .mode = 0,
        .spics_io_num = BSP_NAND_CS,
        .queue_size = active.queue_size,
        .flags = active.flags,
    };
    esp_err_t ret = spi_bus_add_device(BSP_NAND_SPI_HOST, &device_config, &s_spi_device);
    if (ret != ESP_OK) {
        release_resources();
        return ret;
    }
    spi_nand_flash_config_t flash_config = {
        .device_handle = s_spi_device,
        .gc_factor = active.gc_factor,
        .io_mode = active.io_mode,
        .flags = active.flags,
    };
    ret = spi_nand_flash_init_device(&flash_config, &s_flash);
    if (ret != ESP_OK) {
        release_resources();
        return ret;
    }
    if (out_handle) {
        *out_handle = s_flash;
    }
    ESP_LOGI(TAG, "SPI NAND initialized on SPI3: CLK=%d CS=%d D=%d Q=%d HOLD=%d WP=%d mode=%d freq=%d",
             BSP_NAND_CLK, BSP_NAND_CS, BSP_NAND_D, BSP_NAND_Q, BSP_NAND_HOLD, BSP_NAND_WP,
             active.io_mode, active.clock_speed_hz);
    return ESP_OK;
}

esp_err_t bsp_nand_flash_deinit(void)
{
    s_power_save = false;
    return release_resources();
}

spi_nand_flash_device_t *bsp_nand_flash_get_handle(void) { return s_flash; }

esp_err_t bsp_nand_flash_enter_power_save(void)
{
    if (!s_flash || !s_spi_device) {
        return ESP_OK;
    }
    if (s_power_save) {
        return ESP_OK;
    }

    esp_err_t ret = spi_nand_flash_sync(s_flash);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI NAND sync before power-save failed: %s", esp_err_to_name(ret));
    }

    s_power_save = true;
    ESP_LOGI(TAG, "SPI NAND entered standby (CS idle high)");
    return ESP_OK;
}

esp_err_t bsp_nand_flash_exit_power_save(void)
{
    if (!s_flash || !s_spi_device || !s_power_save) {
        return ESP_OK;
    }
    s_power_save = false;
    ESP_LOGI(TAG, "SPI NAND left standby");
    return ESP_OK;
}
