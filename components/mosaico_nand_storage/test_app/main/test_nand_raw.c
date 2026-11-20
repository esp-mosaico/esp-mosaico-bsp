/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nand.h"
#include "nand_private/nand_impl_wrap.h"
#include "nand_test_common.h"
#include "spi_nand_oper.h"
#include "unity.h"

#define TEST_CLOCK_HZ 40000000U

static nand_test_raw_t s_raw;

static void open_raw(void)
{
    TEST_ESP_OK(nand_test_raw_open(&s_raw, TEST_CLOCK_HZ, SPI_NAND_IO_MODE_SIO, true));
}

static uint32_t test_block(void)
{
    uint32_t block = 0;
    TEST_ESP_OK(nand_test_find_good_block(&s_raw, s_raw.block_count, &block));
    return block;
}

static void adjacent_good_blocks(uint32_t *lower, uint32_t *upper)
{
    TEST_ASSERT_NOT_NULL(lower);
    TEST_ASSERT_NOT_NULL(upper);
    for (uint32_t candidate = s_raw.block_count - 1; candidate > 1; --candidate) {
        bool lower_bad = false;
        bool upper_bad = false;
        TEST_ESP_OK(nand_wrap_is_bad(s_raw.flash, candidate - 1, &lower_bad));
        TEST_ESP_OK(nand_wrap_is_bad(s_raw.flash, candidate, &upper_bad));
        if (!lower_bad && !upper_bad) {
            *lower = candidate - 1;
            *upper = candidate;
            return;
        }
    }
    TEST_FAIL_MESSAGE("No adjacent good blocks available");
}

static void *page_buffer(void)
{
    void *buffer = heap_caps_malloc(s_raw.page_size, MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(buffer);
    return buffer;
}

TEST_CASE("P0 JEDEC ID and geometry", "[nand][raw][p0][auto]")
{
    open_raw();
    TEST_ASSERT_NOT_EQUAL(0, nand_test_manufacturer_id(&s_raw));
    TEST_ASSERT_NOT_EQUAL(0, nand_test_device_id(&s_raw));
    TEST_ASSERT_TRUE(strlen(nand_test_chip_name(&s_raw)) > 0);
    TEST_ASSERT_EQUAL_UINT32(2048, s_raw.page_size);
    TEST_ASSERT_EQUAL_UINT32(64, s_raw.pages_per_block);
    printf("chip=%s manufacturer=0x%02x device=0x%04x blocks=%" PRIu32 " page=%" PRIu32 "\n",
           nand_test_chip_name(&s_raw), nand_test_manufacturer_id(&s_raw), nand_test_device_id(&s_raw), s_raw.block_count, s_raw.page_size);
}

TEST_CASE("P0 reset and busy status", "[nand][raw][p0][auto]")
{
    open_raw();
    uint32_t block = test_block();
    uint8_t status = 0;
    uint32_t elapsed_us = 0;
    TEST_ESP_OK(nand_test_load_page(&s_raw, block * s_raw.pages_per_block, &status, &elapsed_us));
    TEST_ASSERT_EQUAL_HEX8(0, status & STAT_BUSY);
    TEST_ESP_OK(nand_test_reset(&s_raw));
    TEST_ESP_OK(spi_nand_read_register(s_raw.flash, REG_STATUS, &status));
    TEST_ASSERT_EQUAL_HEX8(0, status & STAT_BUSY);
    printf("page_load_us=%" PRIu32 " final_status=0x%02x\n", elapsed_us, status);
}

TEST_CASE("P0 page program read and block erase", "[nand][raw][p0][auto][destructive]")
{
    open_raw();
    uint32_t block = test_block();
    uint32_t page = block * s_raw.pages_per_block;
    uint8_t *expected = page_buffer();
    uint8_t *actual = page_buffer();
    nand_test_fill_random(expected, s_raw.page_size, 0x12345678);
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, block));
    TEST_ESP_OK(nand_wrap_prog(s_raw.flash, page, expected));
    TEST_ESP_OK(nand_wrap_read(s_raw.flash, page, 0, s_raw.page_size, actual));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, s_raw.page_size);
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, block));
    TEST_ESP_OK(nand_wrap_read(s_raw.flash, page, 0, s_raw.page_size, actual));
    for (uint32_t i = 0; i < s_raw.page_size; ++i) TEST_ASSERT_EQUAL_HEX8(0xFF, actual[i]);
    free(expected);
    free(actual);
}

TEST_CASE("P0 cross-page and cross-block access", "[nand][raw][p0][auto][destructive]")
{
    open_raw();
    uint32_t lower = 0;
    uint32_t upper = 0;
    adjacent_good_blocks(&lower, &upper);
    uint32_t pages[] = {lower * s_raw.pages_per_block + s_raw.pages_per_block - 1, upper * s_raw.pages_per_block};
    uint8_t *expected = page_buffer();
    uint8_t *actual = page_buffer();
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, lower));
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, upper));
    for (size_t i = 0; i < 2; ++i) {
        nand_test_fill_random(expected, s_raw.page_size, 0xA5000000U + (uint32_t)i);
        TEST_ESP_OK(nand_wrap_prog(s_raw.flash, pages[i], expected));
        TEST_ESP_OK(nand_wrap_read(s_raw.flash, pages[i], 0, s_raw.page_size, actual));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, s_raw.page_size);
    }
    free(expected);
    free(actual);
}

TEST_CASE("P0 fixed data patterns", "[nand][raw][p0][auto][destructive]")
{
    open_raw();
    const uint8_t patterns[] = {0x00, 0xFF, 0xAA, 0x55};
    uint32_t block = test_block();
    uint32_t base = block * s_raw.pages_per_block;
    uint8_t *expected = page_buffer();
    uint8_t *actual = page_buffer();
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, block));
    for (size_t i = 0; i < sizeof(patterns); ++i) {
        memset(expected, patterns[i], s_raw.page_size);
        TEST_ESP_OK(nand_wrap_prog(s_raw.flash, base + (uint32_t)i, expected));
        TEST_ESP_OK(nand_wrap_read(s_raw.flash, base + (uint32_t)i, 0, s_raw.page_size, actual));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, s_raw.page_size);
    }
    free(expected);
    free(actual);
}

TEST_CASE("P0 deterministic random data", "[nand][raw][p0][auto][destructive]")
{
    open_raw();
    uint32_t block = test_block();
    uint32_t base = block * s_raw.pages_per_block;
    uint8_t *expected = page_buffer();
    uint8_t *actual = page_buffer();
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, block));
    for (uint32_t page = 0; page < 16; ++page) {
        nand_test_fill_random(expected, s_raw.page_size, 0xC0010000U + page);
        TEST_ESP_OK(nand_wrap_prog(s_raw.flash, base + page, expected));
        TEST_ESP_OK(nand_wrap_read(s_raw.flash, base + page, 0, s_raw.page_size, actual));
        TEST_ASSERT_EQUAL_UINT32(nand_test_crc32(expected, s_raw.page_size), nand_test_crc32(actual, s_raw.page_size));
    }
    free(expected);
    free(actual);
}

TEST_CASE("P0 unaligned partial reads", "[nand][raw][p0][auto][destructive]")
{
    open_raw();
    uint32_t block = test_block();
    uint32_t page = block * s_raw.pages_per_block;
    const struct { uint16_t offset; uint16_t size; } ranges[] = {{1, 1}, {3, 7}, {63, 65}, {511, 513}, {2041, 7}};
    uint8_t *expected = page_buffer();
    uint8_t *actual = page_buffer();
    nand_test_fill_random(expected, s_raw.page_size, 0x13572468);
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, block));
    TEST_ESP_OK(nand_wrap_prog(s_raw.flash, page, expected));
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        memset(actual, 0, s_raw.page_size);
        TEST_ESP_OK(nand_wrap_read(s_raw.flash, page, ranges[i].offset, ranges[i].size, actual));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected + ranges[i].offset, actual, ranges[i].size);
    }
    free(expected);
    free(actual);
}

TEST_CASE("P0 factory bad-block scan", "[nand][raw][p0][auto]")
{
    open_raw();
    uint32_t bad_count = 0;
    for (uint32_t block = 0; block < s_raw.block_count; ++block) {
        bool bad = false;
        TEST_ESP_OK(nand_wrap_is_bad(s_raw.flash, block, &bad));
        if (bad) {
            ++bad_count;
            printf("factory_bad_block=%" PRIu32 "\n", block);
        }
    }
    TEST_ASSERT_LESS_THAN_UINT32(s_raw.block_count, bad_count);
    printf("factory_bad_count=%" PRIu32 " of %" PRIu32 "\n", bad_count, s_raw.block_count);
}

TEST_CASE("P0 ECC status decoding", "[nand][raw][p0][auto]")
{
    open_raw();
    TEST_ASSERT_NOT_NULL(s_raw.flash->ecc_2bit_status_map);
    TEST_ASSERT_EQUAL(NAND_ECC_OK, s_raw.flash->ecc_2bit_status_map[0]);
    TEST_ASSERT_EQUAL(NAND_ECC_1_TO_4_BITS_CORRECTED, s_raw.flash->ecc_2bit_status_map[1]);
    TEST_ASSERT_EQUAL(NAND_ECC_NOT_CORRECTED, s_raw.flash->ecc_2bit_status_map[2]);
    TEST_ASSERT_EQUAL(NAND_ECC_5_TO_8_BITS_CORRECTED, s_raw.flash->ecc_2bit_status_map[3]);
}

TEST_CASE("P0 ECC corrected and uncorrectable errors", "[nand][raw][p0][manual][destructive]")
{
    static uint8_t expected[2048];
    static uint8_t actual[2048];
    open_raw();
    if (nand_test_manufacturer_id(&s_raw) != 0x5E) TEST_IGNORE_MESSAGE("ECC injection is validated for Zbit only");
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(expected), s_raw.page_size);
    uint32_t block = test_block();
    uint32_t corrected_page = block * s_raw.pages_per_block + 6;
    uint32_t failed_page = corrected_page + 1;
    uint8_t config = 0;
    uint8_t status = 0;
    uint8_t one_bit = 0xFE;
    uint8_t sixteen_bits[2] = {0x00, 0x00};
    nand_test_fill_random(expected, s_raw.page_size, 0xECC01234);
    expected[0] = 0xFF;
    expected[1] = 0xFF;
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, block));
    TEST_ESP_OK(nand_wrap_prog(s_raw.flash, corrected_page, expected));
    TEST_ESP_OK(nand_wrap_prog(s_raw.flash, failed_page, expected));
    TEST_ESP_OK(spi_nand_read_register(s_raw.flash, REG_CONFIG, &config));
    TEST_ESP_OK(spi_nand_write_register(s_raw.flash, REG_CONFIG, config & ~CFG_ECC_ENABLE));
    esp_err_t result = nand_test_load_page(&s_raw, corrected_page, NULL, NULL);
    if (result == ESP_OK) result = nand_test_program_segment(&s_raw, corrected_page, 0, &one_bit, sizeof(one_bit), &status);
    if (result == ESP_OK && (status & STAT_PROGRAM_FAILED)) {
        TEST_ESP_OK(spi_nand_write_register(s_raw.flash, REG_CONFIG, config));
        TEST_IGNORE_MESSAGE("Zbit rejected repeat programming; hardware ECC injection is unavailable");
    }
    if (result == ESP_OK) result = nand_test_load_page(&s_raw, failed_page, NULL, NULL);
    if (result == ESP_OK) result = nand_test_program_segment(&s_raw, failed_page, 0, sixteen_bits, sizeof(sixteen_bits), &status);
    if (result == ESP_OK && (status & STAT_PROGRAM_FAILED)) {
        TEST_ESP_OK(spi_nand_write_register(s_raw.flash, REG_CONFIG, config));
        TEST_IGNORE_MESSAGE("Zbit rejected repeat programming; hardware ECC injection is unavailable");
    }
    if (result == ESP_OK) result = nand_test_load_page(&s_raw, corrected_page, NULL, NULL);
    if (result == ESP_OK) result = nand_test_read_cache(&s_raw, 0, actual, 2);
    if (result == ESP_OK && (actual[0] != one_bit || actual[1] != expected[1])) result = ESP_ERR_INVALID_RESPONSE;
    if (result == ESP_OK) result = nand_test_load_page(&s_raw, failed_page, NULL, NULL);
    if (result == ESP_OK) result = nand_test_read_cache(&s_raw, 0, actual, 2);
    if (result == ESP_OK && memcmp(actual, sixteen_bits, sizeof(sixteen_bits)) != 0) result = ESP_ERR_INVALID_RESPONSE;
    esp_err_t restore = spi_nand_write_register(s_raw.flash, REG_CONFIG, config);
    TEST_ESP_OK(restore);
    TEST_ESP_OK(result);
    TEST_ESP_OK(nand_wrap_read(s_raw.flash, corrected_page, 0, s_raw.page_size, actual));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, s_raw.page_size);
    TEST_ESP_OK(spi_nand_read_register(s_raw.flash, REG_STATUS, &status));
    printf("ecc_corrected_status=0x%02x mapped=%u\n", status, nand_test_ecc_status(&s_raw));
    TEST_ASSERT_TRUE(nand_test_ecc_status(&s_raw) == NAND_ECC_1_TO_4_BITS_CORRECTED || nand_test_ecc_status(&s_raw) == NAND_ECC_5_TO_8_BITS_CORRECTED);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nand_wrap_read(s_raw.flash, failed_page, 0, s_raw.page_size, actual));
    TEST_ASSERT_EQUAL(NAND_ECC_NOT_CORRECTED, nand_test_ecc_status(&s_raw));
}

TEST_CASE("P0 program and erase failure reporting", "[nand][raw][p0][auto][destructive]")
{
    open_raw();
    uint32_t block = test_block();
    uint32_t page = block * s_raw.pages_per_block;
    uint8_t protection = 0;
    uint8_t *data = page_buffer();
    memset(data, 0xA5, s_raw.page_size);
    TEST_ESP_OK(spi_nand_read_register(s_raw.flash, REG_PROTECT, &protection));
    TEST_ESP_OK(spi_nand_write_register(s_raw.flash, REG_PROTECT, 0x38));
    esp_err_t erase_result = nand_wrap_erase_block(s_raw.flash, block);
    esp_err_t program_result = nand_wrap_prog(s_raw.flash, page, data);
    TEST_ESP_OK(spi_nand_write_register(s_raw.flash, REG_PROTECT, protection));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, erase_result);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, program_result);
    free(data);
}

TEST_CASE("P1 OOB spare-area read write", "[nand][raw][p1][auto][destructive]")
{
    open_raw();
    uint32_t block = test_block();
    uint32_t page = block * s_raw.pages_per_block + 3;
    const uint8_t expected[8] = {0xF0, 0x0F, 0xAA, 0x55, 0x12, 0x34, 0x56, 0x78};
    uint8_t actual[sizeof(expected)] = {0};
    uint8_t status = 0;
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, block));
    TEST_ESP_OK(nand_test_program_segment(&s_raw, page, (uint16_t)(s_raw.page_size + 8), expected, sizeof(expected), &status));
    TEST_ASSERT_EQUAL_HEX8(0, status & STAT_PROGRAM_FAILED);
    TEST_ESP_OK(nand_test_load_page(&s_raw, page, &status, NULL));
    TEST_ESP_OK(nand_test_read_cache(&s_raw, (uint16_t)(s_raw.page_size + 8), actual, sizeof(actual)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, sizeof(expected));
}

TEST_CASE("P1 illegal repeated programming is detected or characterized", "[nand][raw][p1][manual][destructive]")
{
    open_raw();
    uint32_t block = test_block();
    uint32_t page = block * s_raw.pages_per_block + 4;
    uint8_t *first = page_buffer();
    uint8_t *second = page_buffer();
    memset(first, 0xAA, s_raw.page_size);
    memset(second, 0x55, s_raw.page_size);
    TEST_ESP_OK(nand_wrap_erase_block(s_raw.flash, block));
    TEST_ESP_OK(nand_wrap_prog(s_raw.flash, page, first));
    esp_err_t result = nand_wrap_prog(s_raw.flash, page, second);
    printf("repeat_program_result=%s (0x%x)\n", esp_err_to_name(result), result);
    TEST_ASSERT_TRUE(result == ESP_OK || result == ESP_ERR_NOT_FINISHED);
    free(first);
    free(second);
}

TEST_CASE("P1 SPI clocks modes and DMA", "[nand][raw][p1][manual][destructive]")
{
    const uint32_t clocks[] = {20000000U, 40000000U, 80000000U};
    const spi_nand_flash_io_mode_t modes[] = {SPI_NAND_IO_MODE_SIO, SPI_NAND_IO_MODE_QIO};
    for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
        for (size_t clock = 0; clock < sizeof(clocks) / sizeof(clocks[0]); ++clock) {
            nand_test_raw_t raw = {0};
            TEST_ESP_OK(nand_test_raw_open(&raw, clocks[clock], modes[mode], true));
            uint32_t block = 0;
            TEST_ESP_OK(nand_test_find_good_block(&raw, raw.block_count, &block));
            uint8_t *expected = heap_caps_aligned_alloc(64, raw.page_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            uint8_t *actual = heap_caps_aligned_alloc(64, raw.page_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            TEST_ASSERT_NOT_NULL(expected);
            TEST_ASSERT_NOT_NULL(actual);
            nand_test_fill_random(expected, raw.page_size, clocks[clock] ^ (uint32_t)modes[mode]);
            TEST_ESP_OK(nand_wrap_erase_block(raw.flash, block));
            TEST_ESP_OK(nand_wrap_prog(raw.flash, block * raw.pages_per_block, expected));
            TEST_ESP_OK(nand_wrap_read(raw.flash, block * raw.pages_per_block, 0, raw.page_size, actual));
            TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, raw.page_size);
            printf("mode=%d clock=%" PRIu32 " passed\n", modes[mode], clocks[clock]);
            free(expected);
            free(actual);
            nand_test_raw_close(&raw);
        }
    }
}
