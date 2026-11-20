/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "nand_test_common.h"

#include <inttypes.h>
#include <string.h>
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "mosaico_nand_extension.h"
#include "nand.h"
#include "nand_impl.h"
#include "nand_private/nand_impl_wrap.h"
#include "spi_nand_oper.h"

#define NAND_TEST_BOOTSTRAP_HZ 20000000U
#define NAND_TEST_READY_TIMEOUT_US 100000U

static nand_test_raw_t *s_active;

static esp_err_t configure_control_pin(gpio_num_t pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 1), "nand_test", "preload control pin");
    ESP_RETURN_ON_ERROR(gpio_config(&config), "nand_test", "configure control pin");
    return gpio_set_level(pin, 1);
}

static esp_err_t add_bus(nand_test_raw_t *ctx, uint32_t clock_hz, bool quad)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = BSP_NAND_D,
        .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK,
        .quadhd_io_num = quad ? BSP_NAND_HOLD : GPIO_NUM_NC,
        .quadwp_io_num = quad ? BSP_NAND_WP : GPIO_NUM_NC,
        .max_transfer_sz = 4096,
        .data_io_default_level = true,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO), "nand_test", "initialize SPI bus");
    ctx->bus_initialized = true;
    const spi_device_interface_config_t device = {
        .command_bits = 8,
        .clock_speed_hz = clock_hz,
        .mode = 0,
        .spics_io_num = BSP_NAND_CS,
        .queue_size = 10,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    esp_err_t err = spi_bus_add_device(BSP_NAND_SPI_HOST, &device, &ctx->spi);
    if (err != ESP_OK) nand_test_raw_close(ctx);
    return err;
}

static esp_err_t prepare_quad(nand_test_raw_t *ctx)
{
    ESP_RETURN_ON_ERROR(add_bus(ctx, NAND_TEST_BOOTSTRAP_HZ, false), "nand_test", "add bootstrap bus");
    esp_err_t err = mosaico_nand_prepare_quad(ctx->spi);
    if (ctx->spi) {
        esp_err_t cleanup = spi_bus_remove_device(ctx->spi);
        ctx->spi = NULL;
        if (err == ESP_OK) err = cleanup;
    }
    if (ctx->bus_initialized) {
        esp_err_t cleanup = spi_bus_free(BSP_NAND_SPI_HOST);
        ctx->bus_initialized = false;
        if (err == ESP_OK) err = cleanup;
    }
    return err;
}

esp_err_t nand_test_raw_open(nand_test_raw_t *ctx, uint32_t clock_hz, spi_nand_flash_io_mode_t mode, bool poll_short)
{
    if (!ctx || !clock_hz || clock_hz > 80000000U || s_active) return ESP_ERR_INVALID_ARG;
    memset(ctx, 0, sizeof(*ctx));
    ESP_RETURN_ON_ERROR(configure_control_pin(BSP_NAND_HOLD), "nand_test", "release HOLD");
    ESP_RETURN_ON_ERROR(configure_control_pin(BSP_NAND_WP), "nand_test", "release WP");
    bool quad = mode == SPI_NAND_IO_MODE_QIO || mode == SPI_NAND_IO_MODE_QOUT;
    if (quad) ESP_RETURN_ON_ERROR(prepare_quad(ctx), "nand_test", "prepare quad mode");
    ESP_RETURN_ON_ERROR(add_bus(ctx, clock_hz, quad), "nand_test", "add test bus");
    spi_nand_flash_config_t config = {
        .device_handle = ctx->spi,
        .io_mode = mode,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .poll_short_commands = poll_short,
    };
    esp_err_t err = nand_init_device(&config, &ctx->flash);
    if (err != ESP_OK) {
        nand_test_raw_close(ctx);
        return err;
    }
    ctx->page_size = ctx->flash->chip.page_size;
    ctx->pages_per_block = 1U << ctx->flash->chip.log2_ppb;
    ctx->block_count = ctx->flash->chip.num_blocks;
    s_active = ctx;
    return ESP_OK;
}

void nand_test_raw_close(nand_test_raw_t *ctx)
{
    if (!ctx) return;
    if (ctx->flash) {
        (void)spi_nand_flash_deinit_device(ctx->flash);
        ctx->flash = NULL;
    }
    if (ctx->spi) {
        (void)spi_bus_remove_device(ctx->spi);
        ctx->spi = NULL;
    }
    if (ctx->bus_initialized) {
        (void)spi_bus_free(BSP_NAND_SPI_HOST);
        ctx->bus_initialized = false;
    }
    if (s_active == ctx) s_active = NULL;
}

void nand_test_cleanup(void)
{
    if (s_active) nand_test_raw_close(s_active);
}

esp_err_t nand_test_wait_ready(nand_test_raw_t *ctx, uint32_t timeout_us, uint8_t *status, uint32_t *elapsed_us)
{
    if (!ctx || !ctx->flash || !timeout_us) return ESP_ERR_INVALID_ARG;
    int64_t start = esp_timer_get_time();
    uint8_t current = 0;
    do {
        ESP_RETURN_ON_ERROR(spi_nand_read_register(ctx->flash, REG_STATUS, &current), "nand_test", "read status");
        if (!(current & STAT_BUSY)) {
            if (status) *status = current;
            if (elapsed_us) *elapsed_us = (uint32_t)(esp_timer_get_time() - start);
            return ESP_OK;
        }
        esp_rom_delay_us(10);
    } while ((uint64_t)(esp_timer_get_time() - start) < timeout_us);
    if (status) *status = current;
    if (elapsed_us) *elapsed_us = (uint32_t)(esp_timer_get_time() - start);
    return ESP_ERR_TIMEOUT;
}

esp_err_t nand_test_reset(nand_test_raw_t *ctx)
{
    if (!ctx || !ctx->flash) return ESP_ERR_INVALID_ARG;
    spi_nand_transaction_t transaction = {.command = 0xFF};
    ESP_RETURN_ON_ERROR(spi_nand_execute_transaction(ctx->flash, &transaction), "nand_test", "send reset");
    return nand_test_wait_ready(ctx, NAND_TEST_READY_TIMEOUT_US, NULL, NULL);
}

esp_err_t nand_test_load_page(nand_test_raw_t *ctx, uint32_t page, uint8_t *status, uint32_t *elapsed_us)
{
    if (!ctx || !ctx->flash || page >= ctx->block_count * ctx->pages_per_block) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(spi_nand_read_page(ctx->flash, page), "nand_test", "load page");
    return nand_test_wait_ready(ctx, NAND_TEST_READY_TIMEOUT_US, status, elapsed_us);
}

esp_err_t nand_test_program_segment(nand_test_raw_t *ctx, uint32_t page, uint16_t column, const void *data, size_t size, uint8_t *status)
{
    if (!ctx || !ctx->flash || !data || !size || size > UINT16_MAX || page >= ctx->block_count * ctx->pages_per_block) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(spi_nand_write_enable(ctx->flash), "nand_test", "write enable");
    ESP_RETURN_ON_ERROR(spi_nand_program_load(ctx->flash, data, column, (uint16_t)size), "nand_test", "program load");
    ESP_RETURN_ON_ERROR(spi_nand_program_execute(ctx->flash, page), "nand_test", "program execute");
    return nand_test_wait_ready(ctx, NAND_TEST_READY_TIMEOUT_US, status, NULL);
}

esp_err_t nand_test_read_cache(nand_test_raw_t *ctx, uint16_t column, void *data, size_t size)
{
    if (!ctx || !ctx->flash || !data || !size || size > UINT16_MAX) return ESP_ERR_INVALID_ARG;
    return spi_nand_read(ctx->flash, data, column, (uint16_t)size);
}

esp_err_t nand_test_find_good_block(nand_test_raw_t *ctx, uint32_t before_block, uint32_t *block)
{
    if (!ctx || !ctx->flash || !block || !before_block || before_block > ctx->block_count) return ESP_ERR_INVALID_ARG;
    for (uint32_t candidate = before_block; candidate-- > 1;) {
        bool bad = false;
        ESP_RETURN_ON_ERROR(nand_wrap_is_bad(ctx->flash, candidate, &bad), "nand_test", "read bad-block marker");
        if (!bad) {
            *block = candidate;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void nand_test_fill_random(void *data, size_t size, uint32_t seed)
{
    uint8_t *bytes = data;
    uint32_t state = seed ? seed : 1;
    for (size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[i] = (uint8_t)state;
    }
}

uint32_t nand_test_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

const char *nand_test_chip_name(const nand_test_raw_t *ctx) { return ctx && ctx->flash ? ctx->flash->device_info.chip_name : ""; }
uint8_t nand_test_manufacturer_id(const nand_test_raw_t *ctx) { return ctx && ctx->flash ? ctx->flash->device_info.manufacturer_id : 0; }
uint16_t nand_test_device_id(const nand_test_raw_t *ctx) { return ctx && ctx->flash ? ctx->flash->device_info.device_id : 0; }
uint8_t nand_test_ecc_status(const nand_test_raw_t *ctx) { return ctx && ctx->flash ? ctx->flash->chip.ecc_data.ecc_corrected_bits_status : NAND_ECC_MAX; }
