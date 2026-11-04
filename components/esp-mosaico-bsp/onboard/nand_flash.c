/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mosaico_nand_extension.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "S31-Mosaico-NAND";
static spi_device_handle_t s_spi_device;
static spi_nand_flash_device_t *s_flash;
static bool s_bus_initialized;
static bool s_power_save;

#define NAND_BOOTSTRAP_MAX_CLOCK_HZ (20 * 1000 * 1000)

/* Serialize lifecycle operations without holding a spinlock during SPI IO.
 * Callers still own synchronization against live filesystem/page users. */
static atomic_flag s_lifecycle = ATOMIC_FLAG_INIT;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
static esp_blockdev_handle_t s_blockdev;
static esp_blockdev_ops_t s_blockdev_ops;
static esp_err_t (*s_blockdev_release)(esp_blockdev_handle_t);
#endif

static bool begin_lifecycle(void)
{
    return !atomic_flag_test_and_set(&s_lifecycle);
}

static void end_lifecycle(void)
{
    atomic_flag_clear(&s_lifecycle);
}

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
        if (ret == ESP_OK) s_spi_device = NULL;
    }
    if (s_bus_initialized && !s_spi_device) {
        esp_err_t ret = spi_bus_free(BSP_NAND_SPI_HOST);
        if (first_error == ESP_OK) {
            first_error = ret;
        }
        if (ret == ESP_OK) s_bus_initialized = false;
    }
    return first_error;
}

static esp_err_t configure_control_pin(gpio_num_t pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 1), TAG, "preload NAND control GPIO%d failed", pin);
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure NAND control GPIO%d failed", pin);
    return gpio_set_level(pin, 1);
}

static esp_err_t bootstrap_quad_mode(const spi_device_interface_config_t *device_config, int max_transfer_sz)
{
    /* IO2/IO3 are still WP/HOLD until QE is enabled. Use a low single-IO
     * clock for detection/configuration, preserving slower caller settings. */
    spi_device_interface_config_t bootstrap_config = *device_config;
    if (bootstrap_config.clock_speed_hz > NAND_BOOTSTRAP_MAX_CLOCK_HZ) {
        bootstrap_config.clock_speed_hz = NAND_BOOTSTRAP_MAX_CLOCK_HZ;
    }
    const spi_bus_config_t bus_config = {
        .mosi_io_num = BSP_NAND_D,
        .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK,
        .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .max_transfer_sz = max_transfer_sz,
    };
    esp_err_t ret = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return ret;
    }
    s_bus_initialized = true;
    ret = spi_bus_add_device(BSP_NAND_SPI_HOST, &bootstrap_config, &s_spi_device);
    if (ret == ESP_OK) {
        ret = mosaico_nand_prepare_quad(s_spi_device);
    }
    if (ret == ESP_OK) {
        ret = spi_bus_remove_device(s_spi_device);
        if (ret == ESP_OK) {
            s_spi_device = NULL;
        }
    }
    if (ret == ESP_OK) {
        ret = spi_bus_free(BSP_NAND_SPI_HOST);
        if (ret == ESP_OK) {
            s_bus_initialized = false;
        }
    }
    if (ret != ESP_OK) {
        (void)release_resources();
    }
    return ret;
}

static esp_err_t initialize_bus(const bsp_nand_flash_config_t *config,
                                 bsp_nand_flash_config_t *active)
{
    ESP_RETURN_ON_FALSE(!s_spi_device && !s_bus_initialized && !s_flash,
                        ESP_ERR_INVALID_STATE, TAG, "SPI NAND is already initialized");
    *active = config ? *config : (bsp_nand_flash_config_t)BSP_NAND_FLASH_DEFAULT_CONFIG();
    if (!active->clock_speed_hz) active->clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ;
    if (!active->queue_size) active->queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE;
    if (!active->max_transfer_sz) active->max_transfer_sz = BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ;
    ESP_RETURN_ON_FALSE(active->clock_speed_hz > 0 && active->clock_speed_hz <= 80000000 &&
                        active->queue_size > 0 && active->max_transfer_sz > 0 &&
                        active->io_mode >= SPI_NAND_IO_MODE_SIO && active->io_mode <= SPI_NAND_IO_MODE_QIO,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid NAND configuration");
    const bool quad = active->io_mode == SPI_NAND_IO_MODE_QOUT || active->io_mode == SPI_NAND_IO_MODE_QIO;
    ESP_RETURN_ON_ERROR(configure_control_pin(BSP_NAND_HOLD), TAG, "release NAND HOLD failed");
    ESP_RETURN_ON_ERROR(configure_control_pin(BSP_NAND_WP), TAG, "release NAND WP failed");
    const spi_device_interface_config_t device_config = {
        .command_bits = 8,
        .clock_speed_hz = active->clock_speed_hz,
        .mode = 0,
        .spics_io_num = BSP_NAND_CS,
        .queue_size = active->queue_size,
        .flags = active->flags,
    };
    if (quad) {
        ESP_RETURN_ON_ERROR(bootstrap_quad_mode(&device_config, active->max_transfer_sz), TAG,
                            "bootstrap NAND quad mode failed");
    }
    const spi_bus_config_t bus_config = {
        .mosi_io_num = BSP_NAND_D,
        .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK,
        .quadhd_io_num = quad ? BSP_NAND_HOLD : GPIO_NUM_NC,
        .quadwp_io_num = quad ? BSP_NAND_WP : GPIO_NUM_NC,
        .max_transfer_sz = active->max_transfer_sz,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_NAND_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG,
                        "initialize SPI3 NAND bus failed");
    s_bus_initialized = true;
    esp_err_t ret = spi_bus_add_device(BSP_NAND_SPI_HOST, &device_config, &s_spi_device);
    if (ret != ESP_OK) (void)release_resources();
    return ret;
}

static spi_nand_flash_config_t driver_config(const bsp_nand_flash_config_t *active)
{
    return (spi_nand_flash_config_t){
        .device_handle = s_spi_device,
        .gc_factor = active->gc_factor,
        .io_mode = active->io_mode,
        .flags = active->flags,
        .poll_short_commands = active->poll_short_commands,
    };
}

static void log_clock(const bsp_nand_flash_config_t *active)
{
    int actual_khz = 0;
    esp_err_t ret = spi_device_get_actual_freq(s_spi_device, &actual_khz);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPI NAND initialized: mode=%d requested_hz=%d actual_khz=%d",
                 active->io_mode, active->clock_speed_hz, actual_khz);
    } else {
        ESP_LOGW(TAG, "Read SPI NAND clock failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t bsp_nand_flash_init(const bsp_nand_flash_config_t *config, spi_nand_flash_device_t **out_handle)
{
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    (void)config;
    if (out_handle) *out_handle = NULL;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (out_handle) *out_handle = NULL;
    if (!begin_lifecycle()) return ESP_ERR_INVALID_STATE;
    bsp_nand_flash_config_t active;
    esp_err_t ret = initialize_bus(config, &active);
    if (ret == ESP_OK) {
        spi_nand_flash_config_t nand_config = driver_config(&active);
        ret = spi_nand_flash_init_device(&nand_config, &s_flash);
        if (ret != ESP_OK) (void)release_resources();
    }
    if (ret == ESP_OK) {
        if (out_handle) *out_handle = s_flash;
        log_clock(&active);
    }
    end_lifecycle();
    return ret;
#endif
}

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
static esp_err_t release_blockdev(esp_blockdev_handle_t device)
{
    /* VFS release consumes its device even if an error is reported. Finish any
     * concurrent power transition before releasing the owned bus resources. */
    while (!begin_lifecycle()) vTaskDelay(1);
    if (device != s_blockdev) {
        end_lifecycle();
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = s_blockdev_release(device);
    s_blockdev = NULL;
    s_blockdev_release = NULL;
    s_power_save = false;
    esp_err_t cleanup = release_resources();
    end_lifecycle();
    return ret == ESP_OK ? cleanup : ret;
}
#endif

esp_err_t bsp_nand_flash_init_blockdev(const bsp_nand_flash_config_t *config,
                                     esp_blockdev_handle_t *out_handle)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;
    *out_handle = NULL;
#ifndef CONFIG_NAND_FLASH_ENABLE_BDL
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!begin_lifecycle()) return ESP_ERR_INVALID_STATE;
    bsp_nand_flash_config_t active;
    esp_err_t ret = initialize_bus(config, &active);
    if (ret == ESP_OK) {
        spi_nand_flash_config_t nand_config = driver_config(&active);
        ret = spi_nand_flash_init_with_layers(&nand_config, &s_blockdev);
        if (ret != ESP_OK) (void)release_resources();
    }
    if (ret == ESP_OK) {
        /* The board owns one NAND. Preserve the driver's context and operations,
         * adding a release hook so VFS ownership also covers the SPI resources. */
        s_blockdev_ops = *s_blockdev->ops;
        s_blockdev_release = s_blockdev_ops.release;
        s_blockdev_ops.release = release_blockdev;
        s_blockdev->ops = &s_blockdev_ops;
        *out_handle = s_blockdev;
        log_clock(&active);
    }
    end_lifecycle();
    return ret;
#endif
}

esp_err_t bsp_nand_flash_deinit(void)
{
    if (!begin_lifecycle()) return ESP_ERR_INVALID_STATE;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    if (s_blockdev) {
        end_lifecycle();
        return ESP_ERR_INVALID_STATE;
    }
#endif
    s_power_save = false;
    esp_err_t ret = release_resources();
    end_lifecycle();
    return ret;
}

spi_nand_flash_device_t *bsp_nand_flash_get_handle(void) { return s_flash; }

esp_err_t bsp_nand_flash_enter_power_save(void)
{
    if (!begin_lifecycle()) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    if (!s_power_save) {
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
        if (s_blockdev) ret = s_blockdev->ops->sync(s_blockdev);
#else
        if (s_flash) ret = spi_nand_flash_sync(s_flash);
#endif
        if (ret == ESP_OK && s_spi_device) s_power_save = true;
    }
    end_lifecycle();
    return ret;
}

esp_err_t bsp_nand_flash_exit_power_save(void)
{
    if (!begin_lifecycle()) return ESP_ERR_INVALID_STATE;
    s_power_save = false;
    end_lifecycle();
    return ESP_OK;
}
