/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_nand_storage.h"
#include "nand_private/nand_impl_wrap.h"
#include "nand_test_common.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"

#define RELIABILITY_FILE "/nand/reliability.bin"
#define RELIABILITY_SIZE (256U * 1024U)

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t crc;
    uint32_t seed;
} reliability_record_t;

static void save_record(const char *key, const reliability_record_t *record)
{
    TEST_ESP_OK(nvs_flash_init());
    nvs_handle_t nvs;
    TEST_ESP_OK(nvs_open("nand_test", NVS_READWRITE, &nvs));
    TEST_ESP_OK(nvs_set_blob(nvs, key, record, sizeof(*record)));
    TEST_ESP_OK(nvs_commit(nvs));
    nvs_close(nvs);
}

static reliability_record_t load_record(const char *key)
{
    TEST_ESP_OK(nvs_flash_init());
    nvs_handle_t nvs;
    reliability_record_t record = {0};
    size_t size = sizeof(record);
    TEST_ESP_OK(nvs_open("nand_test", NVS_READONLY, &nvs));
    TEST_ESP_OK(nvs_get_blob(nvs, key, &record, &size));
    nvs_close(nvs);
    TEST_ASSERT_EQUAL_size_t(sizeof(record), size);
    TEST_ASSERT_EQUAL_HEX32(0x4E414E44, record.magic);
    return record;
}

static reliability_record_t write_reliability_file(uint32_t seed)
{
    TEST_ESP_OK(mosaico_nand_storage_format(NULL));
    uint8_t *buffer = malloc(2048);
    TEST_ASSERT_NOT_NULL(buffer);
    FILE *file = fopen(RELIABILITY_FILE, "wb");
    TEST_ASSERT_NOT_NULL(file);
    uint32_t crc = UINT32_MAX;
    for (uint32_t offset = 0; offset < RELIABILITY_SIZE; offset += 2048) {
        nand_test_fill_random(buffer, 2048, seed + offset);
        TEST_ASSERT_EQUAL_size_t(2048, fwrite(buffer, 1, 2048, file));
        crc ^= nand_test_crc32(buffer, 2048) + offset;
    }
    TEST_ASSERT_EQUAL(0, fflush(file));
    TEST_ASSERT_EQUAL(0, fsync(fileno(file)));
    TEST_ASSERT_EQUAL(0, fclose(file));
    free(buffer);
    return (reliability_record_t){.magic = 0x4E414E44, .size = RELIABILITY_SIZE, .crc = crc, .seed = seed};
}

static void verify_reliability_file(const reliability_record_t *record)
{
    TEST_ESP_OK(mosaico_nand_storage_mount(NULL));
    uint8_t *buffer = malloc(2048);
    TEST_ASSERT_NOT_NULL(buffer);
    FILE *file = fopen(RELIABILITY_FILE, "rb");
    TEST_ASSERT_NOT_NULL(file);
    uint32_t crc = UINT32_MAX;
    for (uint32_t offset = 0; offset < record->size; offset += 2048) {
        TEST_ASSERT_EQUAL_size_t(2048, fread(buffer, 1, 2048, file));
        crc ^= nand_test_crc32(buffer, 2048) + offset;
    }
    TEST_ASSERT_EQUAL(0, fclose(file));
    free(buffer);
    TEST_ASSERT_EQUAL_HEX32(record->crc, crc);
}

TEST_CASE("P0 power-loss recovery prepare", "[nand][reliability][p0][manual][destructive][power]")
{
    reliability_record_t record = write_reliability_file(0x504F5745);
    save_record("power", &record);
    printf("POWER_CUT_READY: remove power now, then boot and run the verify test.\n");
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

TEST_CASE("P0 power-loss recovery verify", "[nand][reliability][p0][manual][power]")
{
    reliability_record_t record = load_record("power");
    verify_reliability_file(&record);
}

TEST_CASE("P1 data retention prepare", "[nand][reliability][p1][manual][destructive][retention]")
{
    reliability_record_t record = write_reliability_file(0x52455441);
    save_record("retention", &record);
    printf("RETENTION_READY: power off for the required retention interval, then run the verify test.\n");
}

TEST_CASE("P1 data retention verify", "[nand][reliability][p1][manual][retention]")
{
    reliability_record_t record = load_record("retention");
    verify_reliability_file(&record);
}

TEST_CASE("P1 long random read write", "[nand][reliability][p1][manual][destructive][stress]")
{
    TEST_ESP_OK(mosaico_nand_storage_format(NULL));
    const uint32_t slots = 128;
    const uint32_t operations = 10000;
    uint32_t *seeds = calloc(slots, sizeof(*seeds));
    uint8_t *expected = malloc(2048);
    uint8_t *actual = malloc(2048);
    TEST_ASSERT_NOT_NULL(seeds);
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_NOT_NULL(actual);
    FILE *file = fopen("/nand/random.bin", "w+b");
    TEST_ASSERT_NOT_NULL(file);
    memset(expected, 0xFF, 2048);
    for (uint32_t slot = 0; slot < slots; ++slot) TEST_ASSERT_EQUAL_size_t(2048, fwrite(expected, 1, 2048, file));
    uint32_t state = 0x31415926;
    int64_t max_us = 0;
    for (uint32_t operation = 1; operation <= operations; ++operation) {
        state = state * 1664525U + 1013904223U;
        uint32_t slot = state % slots;
        seeds[slot] = state;
        nand_test_fill_random(expected, 2048, state);
        TEST_ASSERT_EQUAL(0, fseek(file, (long)(slot * 2048U), SEEK_SET));
        int64_t start = esp_timer_get_time();
        TEST_ASSERT_EQUAL_size_t(2048, fwrite(expected, 1, 2048, file));
        TEST_ASSERT_EQUAL(0, fflush(file));
        TEST_ASSERT_EQUAL(0, fseek(file, (long)(slot * 2048U), SEEK_SET));
        TEST_ASSERT_EQUAL_size_t(2048, fread(actual, 1, 2048, file));
        int64_t elapsed = esp_timer_get_time() - start;
        if (elapsed > max_us) max_us = elapsed;
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, 2048);
        if (!(operation % 1000)) printf("stress_progress=%" PRIu32 "/%" PRIu32 " max_us=%" PRId64 "\n", operation, operations, max_us);
    }
    TEST_ASSERT_EQUAL(0, fclose(file));
    free(seeds);
    free(expected);
    free(actual);
}

static uint32_t count_bad_blocks(nand_test_raw_t *raw)
{
    uint32_t count = 0;
    for (uint32_t block = 0; block < raw->block_count; ++block) {
        bool bad = false;
        TEST_ESP_OK(nand_wrap_is_bad(raw->flash, block, &bad));
        if (bad) ++count;
    }
    return count;
}

TEST_CASE("P1 erase endurance and bad-block growth", "[nand][reliability][p1][manual][destructive][endurance]")
{
    nand_test_raw_t raw = {0};
    TEST_ESP_OK(nand_test_raw_open(&raw, 40000000, SPI_NAND_IO_MODE_SIO, true));
    uint32_t block = 0;
    TEST_ESP_OK(nand_test_find_good_block(&raw, raw.block_count, &block));
    uint32_t bad_before = count_bad_blocks(&raw);
    uint8_t *expected = heap_caps_malloc(raw.page_size, MALLOC_CAP_8BIT);
    uint8_t *actual = heap_caps_malloc(raw.page_size, MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_NOT_NULL(actual);
    for (uint32_t cycle = 1; cycle <= 1000; ++cycle) {
        nand_test_fill_random(expected, raw.page_size, cycle);
        TEST_ESP_OK(nand_wrap_erase_block(raw.flash, block));
        TEST_ESP_OK(nand_wrap_prog(raw.flash, block * raw.pages_per_block, expected));
        TEST_ESP_OK(nand_wrap_read(raw.flash, block * raw.pages_per_block, 0, raw.page_size, actual));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, raw.page_size);
        if (!(cycle % 100)) printf("endurance_cycle=%" PRIu32 "/1000\n", cycle);
    }
    uint32_t bad_after = count_bad_blocks(&raw);
    printf("bad_blocks_before=%" PRIu32 " bad_blocks_after=%" PRIu32 "\n", bad_before, bad_after);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(bad_before, bad_after);
    free(expected);
    free(actual);
    nand_test_raw_close(&raw);
}
