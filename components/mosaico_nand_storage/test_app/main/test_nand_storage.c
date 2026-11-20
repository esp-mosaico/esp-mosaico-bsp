/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bsp/nand_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_nand_storage.h"
#include "nand_test_common.h"
#include "unity.h"

#define TEST_PATH "/nand"
#define TEST_CHUNK 2048U

static void format_storage(void)
{
    TEST_ESP_OK(mosaico_nand_storage_format(NULL));
}

static void write_all(FILE *file, const void *data, size_t size)
{
    TEST_ASSERT_EQUAL_size_t(size, fwrite(data, 1, size, file));
}

static void read_all(FILE *file, void *data, size_t size)
{
    TEST_ASSERT_EQUAL_size_t(size, fread(data, 1, size, file));
}

TEST_CASE("P0 block-device address boundaries and arguments", "[nand][storage][p0][auto]")
{
    esp_blockdev_handle_t device = NULL;
    TEST_ESP_OK(bsp_nand_flash_init_blockdev(NULL, &device));
    TEST_ASSERT_NOT_NULL(device);
    uint8_t *buffer = malloc(device->geometry.read_size);
    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device->ops->read(device, NULL, device->geometry.read_size, 0, device->geometry.read_size));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, device->ops->read(device, buffer, device->geometry.read_size, 1, device->geometry.read_size));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, device->ops->read(device, buffer, device->geometry.read_size, device->geometry.disk_size, device->geometry.read_size));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device->ops->write(device, NULL, 0, device->geometry.write_size));
    free(buffer);
    TEST_ESP_OK(device->ops->release(device));
    mosaico_nand_storage_config_t invalid = MOSAICO_NAND_STORAGE_DEFAULT_CONFIG();
    invalid.base_path = "relative";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mosaico_nand_storage_mount(&invalid));
}

TEST_CASE("P1 filesystem format mount unmount and recovery", "[nand][storage][p1][auto][destructive]")
{
    format_storage();
    uint8_t expected[TEST_CHUNK];
    uint8_t actual[TEST_CHUNK];
    nand_test_fill_random(expected, sizeof(expected), 0x10203040);
    FILE *file = fopen(TEST_PATH "/recover.bin", "wb");
    TEST_ASSERT_NOT_NULL(file);
    write_all(file, expected, sizeof(expected));
    TEST_ASSERT_EQUAL(0, fflush(file));
    TEST_ASSERT_EQUAL(0, fsync(fileno(file)));
    TEST_ASSERT_EQUAL(0, fclose(file));
    TEST_ESP_OK(mosaico_nand_storage_unmount());
    TEST_ESP_OK(mosaico_nand_storage_mount(NULL));
    file = fopen(TEST_PATH "/recover.bin", "rb");
    TEST_ASSERT_NOT_NULL(file);
    read_all(file, actual, sizeof(actual));
    TEST_ASSERT_EQUAL(0, fclose(file));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, sizeof(expected));
    TEST_ESP_OK(mosaico_nand_storage_reclaim());
    uint64_t total = 0, used = 0;
    TEST_ESP_OK(mosaico_nand_storage_info(&total, &used, NULL, NULL));
    TEST_ASSERT_TRUE(total > 0);
    TEST_ASSERT_TRUE(used <= total);
    printf("filesystem_total=%" PRIu64 " used=%" PRIu64 "\n", total, used);
}

TEST_CASE("P0 non-aligned filesystem writes", "[nand][storage][p0][auto][destructive]")
{
    format_storage();
    static const size_t chunks[] = {1, 3, 7, 63, 511, 513, 2047, 2053};
    uint8_t *expected = malloc(8192);
    uint8_t *actual = malloc(8192);
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_NOT_NULL(actual);
    nand_test_fill_random(expected, 8192, 0x55667788);
    FILE *file = fopen(TEST_PATH "/unaligned.bin", "wb");
    TEST_ASSERT_NOT_NULL(file);
    size_t written = 0;
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
        write_all(file, expected + written, chunks[i]);
        written += chunks[i];
    }
    TEST_ASSERT_EQUAL(0, fclose(file));
    file = fopen(TEST_PATH "/unaligned.bin", "rb");
    TEST_ASSERT_NOT_NULL(file);
    read_all(file, actual, written);
    TEST_ASSERT_EQUAL(0, fclose(file));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, written);
    free(expected);
    free(actual);
}

typedef struct {
    unsigned index;
    TaskHandle_t parent;
    esp_err_t result;
} worker_t;

static void concurrent_worker(void *arg)
{
    worker_t *worker = arg;
    char path[48];
    snprintf(path, sizeof(path), TEST_PATH "/worker-%u.bin", worker->index);
    uint8_t data[257];
    nand_test_fill_random(data, sizeof(data), 0xABC00000U + worker->index);
    FILE *file = fopen(path, "wb");
    if (!file) {
        worker->result = ESP_FAIL;
        goto done;
    }
    for (unsigned i = 0; i < 64; ++i) {
        if (fwrite(data, 1, sizeof(data), file) != sizeof(data)) {
            worker->result = ESP_FAIL;
            break;
        }
    }
    if (fclose(file) != 0) worker->result = ESP_FAIL;
done:
    xTaskNotifyGive(worker->parent);
    vTaskDelete(NULL);
}

TEST_CASE("P1 concurrent filesystem access", "[nand][storage][p1][auto][destructive]")
{
    format_storage();
    worker_t workers[2] = {{.index = 0, .parent = xTaskGetCurrentTaskHandle(), .result = ESP_OK},
                           {.index = 1, .parent = xTaskGetCurrentTaskHandle(), .result = ESP_OK}};
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(concurrent_worker, "nand_worker_0", 4096, &workers[0], 4, NULL));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(concurrent_worker, "nand_worker_1", 4096, &workers[1], 4, NULL));
    TEST_ASSERT_GREATER_THAN_UINT32(0, ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(30000)));
    TEST_ASSERT_GREATER_THAN_UINT32(0, ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(30000)));
    TEST_ESP_OK(workers[0].result);
    TEST_ESP_OK(workers[1].result);
    for (unsigned worker = 0; worker < 2; ++worker) {
        char path[48];
        snprintf(path, sizeof(path), TEST_PATH "/worker-%u.bin", worker);
        struct stat info;
        TEST_ASSERT_EQUAL(0, stat(path, &info));
        TEST_ASSERT_EQUAL_UINT32(64 * 257, (uint32_t)info.st_size);
    }
}

TEST_CASE("P1 sequential random throughput and maximum latency", "[nand][storage][p1][auto][destructive][performance]")
{
    format_storage();
    const size_t chunks = 512;
    uint8_t *buffer = heap_caps_aligned_alloc(64, TEST_CHUNK, MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(buffer);
    nand_test_fill_random(buffer, TEST_CHUNK, 0xDEADBEEF);
    FILE *file = fopen(TEST_PATH "/perf.bin", "w+b");
    TEST_ASSERT_NOT_NULL(file);
    int64_t write_total = 0, read_total = 0, write_max = 0, read_max = 0;
    for (size_t i = 0; i < chunks; ++i) {
        int64_t start = esp_timer_get_time();
        write_all(file, buffer, TEST_CHUNK);
        int64_t elapsed = esp_timer_get_time() - start;
        write_total += elapsed;
        if (elapsed > write_max) write_max = elapsed;
    }
    TEST_ASSERT_EQUAL(0, fflush(file));
    TEST_ASSERT_EQUAL(0, fsync(fileno(file)));
    for (size_t i = 0; i < 128; ++i) {
        uint32_t index = (uint32_t)((i * 2654435761U) % chunks);
        TEST_ASSERT_EQUAL(0, fseek(file, (long)(index * TEST_CHUNK), SEEK_SET));
        int64_t start = esp_timer_get_time();
        read_all(file, buffer, TEST_CHUNK);
        int64_t elapsed = esp_timer_get_time() - start;
        read_total += elapsed;
        if (elapsed > read_max) read_max = elapsed;
    }
    TEST_ASSERT_EQUAL(0, fclose(file));
    printf("write_kib_s=%.1f write_max_us=%" PRId64 " random_read_kib_s=%.1f read_max_us=%" PRId64 "\n",
           (double)(chunks * TEST_CHUNK) * 1000000.0 / write_total / 1024.0, write_max,
           (double)(128 * TEST_CHUNK) * 1000000.0 / read_total / 1024.0, read_max);
    free(buffer);
}

TEST_CASE("P1 mount cycles memory and deadlock", "[nand][storage][p1][manual][destructive]")
{
    format_storage();
    TEST_ESP_OK(mosaico_nand_storage_unmount());
    size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    for (unsigned i = 0; i < 10; ++i) {
        TEST_ESP_OK(mosaico_nand_storage_mount(NULL));
        TEST_ESP_OK(mosaico_nand_storage_unmount());
    }
    size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    printf("heap_before=%zu heap_after=%zu delta=%zd\n", before, after, (ssize_t)after - (ssize_t)before);
    TEST_ASSERT_TRUE(after + 1024 >= before);
}
