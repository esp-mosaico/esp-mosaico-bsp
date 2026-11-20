/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "spi_nand_flash.h"

typedef struct {
    spi_device_handle_t spi;
    spi_nand_flash_device_t *flash;
    bool bus_initialized;
    uint32_t page_size;
    uint32_t pages_per_block;
    uint32_t block_count;
} nand_test_raw_t;

esp_err_t nand_test_raw_open(nand_test_raw_t *ctx, uint32_t clock_hz, spi_nand_flash_io_mode_t mode, bool poll_short);
void nand_test_raw_close(nand_test_raw_t *ctx);
void nand_test_cleanup(void);
esp_err_t nand_test_reset(nand_test_raw_t *ctx);
esp_err_t nand_test_wait_ready(nand_test_raw_t *ctx, uint32_t timeout_us, uint8_t *status, uint32_t *elapsed_us);
esp_err_t nand_test_load_page(nand_test_raw_t *ctx, uint32_t page, uint8_t *status, uint32_t *elapsed_us);
esp_err_t nand_test_program_segment(nand_test_raw_t *ctx, uint32_t page, uint16_t column, const void *data, size_t size, uint8_t *status);
esp_err_t nand_test_read_cache(nand_test_raw_t *ctx, uint16_t column, void *data, size_t size);
esp_err_t nand_test_find_good_block(nand_test_raw_t *ctx, uint32_t before_block, uint32_t *block);
void nand_test_fill_random(void *data, size_t size, uint32_t seed);
uint32_t nand_test_crc32(const void *data, size_t size);
const char *nand_test_chip_name(const nand_test_raw_t *ctx);
uint8_t nand_test_manufacturer_id(const nand_test_raw_t *ctx);
uint16_t nand_test_device_id(const nand_test_raw_t *ctx);
uint8_t nand_test_ecc_status(const nand_test_raw_t *ctx);
