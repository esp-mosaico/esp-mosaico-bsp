/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "photo_store.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "spi_nand_flash.h"

static const char *TAG = "photo_store";

#define PHOTO_CATALOG_MAGIC   0x50484F54U /* 'PHOT' */
#define PHOTO_CATALOG_VERSION 1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t photo_count;
    uint32_t next_serial;
    uint32_t data_cursor_page;
    uint32_t reserved;
    uint32_t crc32;
} photo_catalog_header_t;

typedef struct __attribute__((packed)) {
    uint32_t serial;
    uint32_t size_bytes;
    uint32_t start_page;
    uint32_t page_count;
    char filename[16];
} photo_catalog_entry_t;

typedef struct {
    spi_nand_flash_device_t *flash;
    SemaphoreHandle_t lock;
    uint8_t *page_buf;
    uint32_t page_size;
    uint32_t nand_page_count;
    uint32_t catalog_page;
    uint32_t data_start_page;
    uint32_t data_end_page;
    photo_catalog_header_t header;
    photo_catalog_entry_t entries[CONFIG_CAMERA_PHOTO_MAX_COUNT];
} photo_store_context_t;

static photo_store_context_t s_store;

static uint32_t catalog_crc(const photo_catalog_header_t *header,
                            const photo_catalog_entry_t *entries,
                            uint16_t count)
{
    photo_catalog_header_t tmp = *header;
    tmp.crc32 = 0;
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
    if (count > 0) {
        crc = esp_rom_crc32_le(crc, (const uint8_t *)entries,
                               count * sizeof(photo_catalog_entry_t));
    }
    return crc;
}

static esp_err_t catalog_write_locked(void)
{
    memset(s_store.page_buf, 0xFF, s_store.page_size);
    photo_catalog_header_t *hdr = (photo_catalog_header_t *)s_store.page_buf;
    *hdr = s_store.header;
    hdr->crc32 = 0;
    hdr->crc32 = catalog_crc(hdr, s_store.entries, hdr->photo_count);

    photo_catalog_entry_t *dst =
        (photo_catalog_entry_t *)(s_store.page_buf + sizeof(*hdr));
    memcpy(dst, s_store.entries,
           hdr->photo_count * sizeof(photo_catalog_entry_t));

    esp_err_t ret = spi_nand_flash_write_page(
        s_store.flash, s_store.page_buf, s_store.catalog_page);
    if (ret != ESP_OK) {
        return ret;
    }
    return spi_nand_flash_sync(s_store.flash);
}

static bool catalog_is_valid(const photo_catalog_header_t *header,
                             const photo_catalog_entry_t *entries)
{
    if (header->magic != PHOTO_CATALOG_MAGIC ||
        header->version != PHOTO_CATALOG_VERSION ||
        header->photo_count > CONFIG_CAMERA_PHOTO_MAX_COUNT) {
        return false;
    }
    const uint32_t stored = header->crc32;
    photo_catalog_header_t tmp = *header;
    tmp.crc32 = 0;
    const uint32_t calc = catalog_crc(&tmp, entries, header->photo_count);
    return stored == calc;
}

static void catalog_reset_locked(void)
{
    memset(&s_store.header, 0, sizeof(s_store.header));
    memset(s_store.entries, 0, sizeof(s_store.entries));
    s_store.header.magic = PHOTO_CATALOG_MAGIC;
    s_store.header.version = PHOTO_CATALOG_VERSION;
    s_store.header.photo_count = 0;
    s_store.header.next_serial = 1;
    s_store.header.data_cursor_page = s_store.data_start_page;
}

static esp_err_t catalog_load_locked(void)
{
    esp_err_t ret = spi_nand_flash_read_page(
        s_store.flash, s_store.page_buf, s_store.catalog_page);
    ESP_RETURN_ON_ERROR(ret, TAG, "read photo catalog failed");

    const photo_catalog_header_t *hdr = (const photo_catalog_header_t *)s_store.page_buf;
    const photo_catalog_entry_t *entries =
        (const photo_catalog_entry_t *)(s_store.page_buf + sizeof(*hdr));

    if (!catalog_is_valid(hdr, entries)) {
        ESP_LOGW(TAG, "Photo catalog missing or invalid; creating a new catalog");
        catalog_reset_locked();
        return catalog_write_locked();
    }

    s_store.header = *hdr;
    memcpy(s_store.entries, entries,
           hdr->photo_count * sizeof(photo_catalog_entry_t));
    if (s_store.header.data_cursor_page < s_store.data_start_page) {
        s_store.header.data_cursor_page = s_store.data_start_page;
    }
    return ESP_OK;
}

static uint32_t pages_for_size(uint32_t size_bytes)
{
    return (size_bytes + s_store.page_size - 1U) / s_store.page_size;
}

esp_err_t photo_store_init(void)
{
    ESP_RETURN_ON_FALSE(!s_store.flash, ESP_ERR_INVALID_STATE, TAG,
                        "photo store already initialized");

    esp_err_t ret = bsp_nand_flash_init(NULL, &s_store.flash);
    ESP_RETURN_ON_ERROR(ret, TAG, "initialize NAND flash failed");

    ret = spi_nand_flash_get_page_count(s_store.flash, &s_store.nand_page_count);
    ESP_RETURN_ON_ERROR(ret, TAG, "get NAND page count failed");

    ret = spi_nand_flash_get_page_size(s_store.flash, &s_store.page_size);
    ESP_RETURN_ON_ERROR(ret, TAG, "get NAND page size failed");

    s_store.catalog_page = CONFIG_CAMERA_PHOTO_NAND_START_PAGE;
    s_store.data_start_page = s_store.catalog_page + 1U;
    s_store.data_end_page = s_store.nand_page_count - 1U;
    if (s_store.data_start_page >= s_store.nand_page_count) {
        ESP_LOGE(TAG, "NAND start page leaves no room for photo data");
        bsp_nand_flash_deinit();
        s_store.flash = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    s_store.page_buf = malloc(s_store.page_size);
    if (!s_store.page_buf) {
        bsp_nand_flash_deinit();
        s_store.flash = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_store.lock = xSemaphoreCreateMutex();
    if (!s_store.lock) {
        free(s_store.page_buf);
        s_store.page_buf = NULL;
        bsp_nand_flash_deinit();
        s_store.flash = NULL;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_store.lock, portMAX_DELAY);
    ret = catalog_load_locked();
    xSemaphoreGive(s_store.lock);
    if (ret != ESP_OK) {
        photo_store_deinit();
        return ret;
    }

    ESP_LOGI(TAG,
             "Photo store ready: catalog_page=%" PRIu32 ", data_pages=[%" PRIu32 ", %" PRIu32 "], "
             "page_size=%" PRIu32 ", photos=%u, next=%04" PRIu32 ".jpg",
             s_store.catalog_page, s_store.data_start_page, s_store.data_end_page,
             s_store.page_size, s_store.header.photo_count,
             s_store.header.next_serial);
    return ESP_OK;
}

void photo_store_deinit(void)
{
    if (s_store.lock) {
        vSemaphoreDelete(s_store.lock);
        s_store.lock = NULL;
    }
    free(s_store.page_buf);
    s_store.page_buf = NULL;
    if (s_store.flash) {
        bsp_nand_flash_deinit();
        s_store.flash = NULL;
    }
    memset(&s_store, 0, sizeof(s_store));
}

uint32_t photo_store_get_count(void)
{
    return s_store.header.photo_count;
}

esp_err_t photo_store_get_info(uint32_t index, photo_store_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(out_info, ESP_ERR_INVALID_ARG, TAG, "output is null");
    ESP_RETURN_ON_FALSE(s_store.flash, ESP_ERR_INVALID_STATE, TAG,
                        "photo store is not initialized");
    ESP_RETURN_ON_FALSE(index < s_store.header.photo_count, ESP_ERR_NOT_FOUND, TAG,
                        "photo index out of range");

    xSemaphoreTake(s_store.lock, portMAX_DELAY);
    const photo_catalog_entry_t *entry = &s_store.entries[index];
    out_info->serial = entry->serial;
    out_info->size_bytes = entry->size_bytes;
    strlcpy(out_info->filename, entry->filename, sizeof(out_info->filename));
    xSemaphoreGive(s_store.lock);
    return ESP_OK;
}

esp_err_t photo_store_save_jpeg(const uint8_t *jpeg_data, size_t jpeg_size,
                                photo_store_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(jpeg_data && jpeg_size > 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid JPEG buffer");
    ESP_RETURN_ON_FALSE(s_store.flash, ESP_ERR_INVALID_STATE, TAG,
                        "photo store is not initialized");

    xSemaphoreTake(s_store.lock, portMAX_DELAY);

    esp_err_t ret = ESP_OK;
    if (s_store.header.photo_count >= CONFIG_CAMERA_PHOTO_MAX_COUNT) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }

    const uint32_t page_count = pages_for_size((uint32_t)jpeg_size);
    const uint32_t start_page = s_store.header.data_cursor_page;
    if (start_page + page_count - 1U > s_store.data_end_page) {
        ESP_LOGE(TAG, "NAND photo region is full");
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }

    for (uint32_t page_idx = 0; page_idx < page_count; ++page_idx) {
        memset(s_store.page_buf, 0xFF, s_store.page_size);
        const size_t offset = page_idx * s_store.page_size;
        const size_t copy_size =
            (jpeg_size - offset >= s_store.page_size)
                ? s_store.page_size
                : (jpeg_size - offset);
        memcpy(s_store.page_buf, jpeg_data + offset, copy_size);

        ret = spi_nand_flash_write_page(
            s_store.flash, s_store.page_buf, start_page + page_idx);
        if (ret != ESP_OK) {
            goto exit;
        }
    }

    ret = spi_nand_flash_sync(s_store.flash);
    if (ret != ESP_OK) {
        goto exit;
    }

    photo_catalog_entry_t *entry = &s_store.entries[s_store.header.photo_count];
    entry->serial = s_store.header.next_serial;
    entry->size_bytes = (uint32_t)jpeg_size;
    entry->start_page = start_page;
    entry->page_count = page_count;
    snprintf(entry->filename, sizeof(entry->filename), "%04" PRIu32 ".jpg",
             entry->serial);

    s_store.header.photo_count++;
    s_store.header.next_serial++;
    s_store.header.data_cursor_page = start_page + page_count;

    ret = catalog_write_locked();
    if (ret != ESP_OK) {
        goto exit;
    }

    if (out_info) {
        out_info->serial = entry->serial;
        out_info->size_bytes = entry->size_bytes;
        strlcpy(out_info->filename, entry->filename, sizeof(out_info->filename));
    }

    ESP_LOGI(TAG, "Saved %s (%u bytes, pages=%" PRIu32 "-%" PRIu32 ")",
             entry->filename, (unsigned)jpeg_size, start_page,
             start_page + page_count - 1U);

exit:
    xSemaphoreGive(s_store.lock);
    return ret;
}

esp_err_t photo_store_load_jpeg(uint32_t index, uint8_t *buffer, size_t buffer_size,
                                size_t *out_size)
{
    ESP_RETURN_ON_FALSE(buffer && out_size, ESP_ERR_INVALID_ARG, TAG,
                        "output buffers are null");
    ESP_RETURN_ON_FALSE(s_store.flash, ESP_ERR_INVALID_STATE, TAG,
                        "photo store is not initialized");
    ESP_RETURN_ON_FALSE(index < s_store.header.photo_count, ESP_ERR_NOT_FOUND, TAG,
                        "photo index out of range");

    xSemaphoreTake(s_store.lock, portMAX_DELAY);

    const photo_catalog_entry_t entry = s_store.entries[index];
    if (entry.size_bytes > buffer_size) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = ESP_OK;
    for (uint32_t page_idx = 0; page_idx < entry.page_count; ++page_idx) {
        ret = spi_nand_flash_read_page(
            s_store.flash, s_store.page_buf, entry.start_page + page_idx);
        if (ret != ESP_OK) {
            break;
        }

        const size_t offset = page_idx * s_store.page_size;
        const size_t copy_size =
            (entry.size_bytes - offset >= s_store.page_size)
                ? s_store.page_size
                : (entry.size_bytes - offset);
        memcpy(buffer + offset, s_store.page_buf, copy_size);
    }

    if (ret == ESP_OK) {
        *out_size = entry.size_bytes;
    }

    xSemaphoreGive(s_store.lock);
    return ret;
}

esp_err_t photo_store_delete(uint32_t index)
{
    ESP_RETURN_ON_FALSE(s_store.flash, ESP_ERR_INVALID_STATE, TAG,
                        "photo store is not initialized");
    ESP_RETURN_ON_FALSE(index < s_store.header.photo_count, ESP_ERR_NOT_FOUND, TAG,
                        "photo index out of range");

    xSemaphoreTake(s_store.lock, portMAX_DELAY);

    const char *filename = s_store.entries[index].filename;
    const uint32_t remaining = s_store.header.photo_count - index - 1U;
    if (remaining > 0) {
        memmove(&s_store.entries[index], &s_store.entries[index + 1U],
                remaining * sizeof(photo_catalog_entry_t));
    }
    s_store.header.photo_count--;

    esp_err_t ret = catalog_write_locked();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Deleted %s, remaining photos=%u", filename,
                 s_store.header.photo_count);
    }

    xSemaphoreGive(s_store.lock);
    return ret;
}
