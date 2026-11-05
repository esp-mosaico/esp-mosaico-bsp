/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "storage_blockdev.h"
#include "mosaico_nand_extension.h"
#include "dhara/error.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "freertos/task.h"
#endif

static atomic_int s_read_error;
static atomic_int s_media_error;
static atomic_uint s_generation;
uint32_t storage_blockdev_generation(void) { return atomic_load(&s_generation); }
esp_err_t storage_blockdev_read_error(void) { return atomic_load(&s_read_error); }
void storage_blockdev_clear_error(void) { atomic_store(&s_read_error, ESP_OK); }
esp_err_t storage_blockdev_media_error(void) { return atomic_load(&s_media_error); }
/* Writes, trims and syncs run Dhara GC, which stops at the oldest
 * uncorrectable page and fails every later mutation until a format. */
static esp_err_t mutation_result(esp_err_t err) {
    if (err == ESP_ERR_FLASH_BASE + DHARA_E_ECC && atomic_exchange(&s_media_error, err) == ESP_OK)
        ESP_LOGE("mosaico_nand_storage", "Uncorrectable NAND page blocks garbage collection; format required");
    return err;
}

typedef struct { uint64_t address, age; } cache_entry_t;
typedef struct {
    esp_blockdev_t device;
    esp_blockdev_handle_t source;
    esp_blockdev_handle_t *owner;
    SemaphoreHandle_t lock;
    cache_entry_t *entries;
    uint8_t *data;
    uint8_t *candidates;
    size_t page_size, count;
    uint32_t candidate_pages;
    uint64_t clock;
} storage_blockdev_t;

static void invalidate(storage_blockdev_t *s) {
    if (s->entries) memset(s->entries, 0, s->count * sizeof(*s->entries));
    s->clock = 0;
}
static void invalidate_range(storage_blockdev_t *s, uint64_t address, size_t size) {
    for (size_t i = 0; i < s->count; ++i) {
        uint64_t page = s->entries[i].address;
        if (s->entries[i].age && page >= address && page - address < size)
            s->entries[i].age = 0;
    }
}
static void candidate_range(storage_blockdev_t *s, uint64_t address, size_t size, bool set) {
    if (!s->candidates) return;
    uint32_t first = address / s->page_size, count = size / s->page_size;
    for (uint32_t p = first; p < first + count && p < s->candidate_pages; ++p) {
        const uint8_t bit = 1U << (p % 8);
        if (set) s->candidates[p / 8] |= bit;
        else s->candidates[p / 8] &= (uint8_t)~bit;
    }
}
static bool valid_range(esp_blockdev_handle_t dev, uint64_t address, size_t size) {
    const storage_blockdev_t *s = dev->ctx;
    return address <= dev->geometry.disk_size && size <= dev->geometry.disk_size - address &&
           address % s->page_size == 0 && size % s->page_size == 0;
}
static esp_err_t read_pages(esp_blockdev_handle_t dev, uint8_t *dst, size_t capacity,
                            uint64_t address, size_t size) {
    storage_blockdev_t *s = dev->ctx;
    if (!dst || capacity < size || !valid_range(dev, address, size)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    for (size_t offset = 0; offset < size; offset += s->page_size) {
        size_t victim = 0, hit = s->count;
        /* A wrap is practically unreachable, but must never create age=0 for
         * a valid entry: zero is the invalid marker. */
        if (s->clock == UINT64_MAX) invalidate(s);
        for (size_t i = 0; i < s->count; ++i) {
            if (s->entries[i].age && s->entries[i].address == address + offset) {
                hit = i;
                break;
            }
            if (s->entries[i].age < s->entries[victim].age) victim = i;
        }
        if (hit < s->count) {
            memcpy(dst + offset, s->data + hit * s->page_size, s->page_size);
            s->entries[hit].age = ++s->clock;
            continue;
        }
        uint8_t *buffer = s->count ? s->data + victim * s->page_size : dst + offset;
        if (s->count) s->entries[victim].age = 0;
        err = s->source->ops->read(s->source, buffer, s->page_size, address + offset, s->page_size);
        if (err != ESP_OK) {
            atomic_store(&s_read_error, err);
            invalidate(s);
            break;
        }
        if (s->count) {
            s->entries[victim] = (cache_entry_t){address + offset, ++s->clock};
            memcpy(dst + offset, buffer, s->page_size);
        }
    }
    xSemaphoreGive(s->lock);
    return err;
}
/* Write-through only. Invalidate the affected pages BEFORE forwarding. Errors
 * and sync drop all cached pages so verification after persistence
 * starts with fresh media reads. No write result is inserted into this cache. */
static esp_err_t write_pages(esp_blockdev_handle_t dev, const uint8_t *src, uint64_t address, size_t size) {
    storage_blockdev_t *s = dev->ctx;
    if (!src || !valid_range(dev, address, size)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    atomic_fetch_add(&s_generation, 1);
    invalidate_range(s, address, size);
    candidate_range(s, address, size, true);
    esp_err_t err = s->source->ops->write ? mutation_result(s->source->ops->write(s->source, src, address, size)) : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) invalidate(s);
    xSemaphoreGive(s->lock);
    return err;
}
static esp_err_t erase_pages(esp_blockdev_handle_t dev, uint64_t address, size_t size) {
    storage_blockdev_t *s = dev->ctx;
    if (!valid_range(dev, address, size)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    atomic_fetch_add(&s_generation, 1);
    invalidate_range(s, address, size);
    esp_err_t err = s->source->ops->erase ? mutation_result(s->source->ops->erase(s->source, address, size)) : ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_OK) candidate_range(s, address, size, false);
    else invalidate(s);
    xSemaphoreGive(s->lock);
    return err;
}
static esp_err_t sync_pages(esp_blockdev_handle_t dev) {
    storage_blockdev_t *s = dev->ctx;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    invalidate(s);
    esp_err_t err = s->source->ops->sync ? mutation_result(s->source->ops->sync(s->source)) : ESP_OK;
    xSemaphoreGive(s->lock);
    return err;
}
static esp_err_t ioctl_pages(esp_blockdev_handle_t dev, const uint8_t command, void *args) {
    storage_blockdev_t *s = dev->ctx;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    atomic_fetch_add(&s_generation, 1);
    invalidate(s);
    esp_err_t err = s->source->ops->ioctl ? mutation_result(s->source->ops->ioctl(s->source, command, args)) : ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_OK && command == MOSAICO_NAND_DISCARD_ALL && s->candidates)
        memset(s->candidates, 0, ((size_t)s->candidate_pages + 7) / 8);
    if (err != ESP_OK) invalidate(s);
    xSemaphoreGive(s->lock);
    return err;
}
static esp_err_t release_pages(esp_blockdev_handle_t dev) {
    storage_blockdev_t *s = dev->ctx;
    /* The VFS owner must quiesce users before releasing, as with the source. */
    esp_err_t err = s->source->ops->release(s->source);
    vSemaphoreDelete(s->lock);
    free(s->entries);
    free(s->data);
    free(s->candidates);
    if (s->owner && *s->owner == dev) *s->owner = NULL;
    free(s);
    return err;
}
static const esp_blockdev_ops_t ops = {
    .read = read_pages, .write = write_pages, .erase = erase_pages,
    .sync = sync_pages, .ioctl = ioctl_pages, .release = release_pages,
};
esp_err_t storage_blockdev_wrap(esp_blockdev_handle_t source, size_t cache_bytes,
                                esp_blockdev_handle_t *out) {
    if (!source || !out || !source->ops || !source->ops->read || !source->ops->release ||
        !source->geometry.read_size) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    atomic_store(&s_media_error, ESP_OK);
    storage_blockdev_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    s->lock = xSemaphoreCreateMutex();
    if (!s->lock) { free(s); return ESP_ERR_NO_MEM; }
    s->source = source;
    s->owner = out;
    s->page_size = source->geometry.read_size;
    s->count = cache_bytes / s->page_size;
    if (s->count) {
        s->entries = heap_caps_calloc(s->count, sizeof(*s->entries), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s->data = heap_caps_malloc(s->count * s->page_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->entries || !s->data) {
            free(s->entries); free(s->data);
            s->entries = NULL; s->data = NULL; s->count = 0;
            ESP_LOGW("mosaico_nand_storage", "NAND read cache allocation failed; using uncached reads");
        }
    }
    s->device = *source;
    s->device.ctx = s;
    s->device.ops = &ops;
#if CONFIG_MOSAICO_NAND_STORAGE_RECLAIM
    uint64_t pages = source->geometry.disk_size / s->page_size;
    if (pages && pages <= UINT32_MAX) {
        s->candidate_pages = pages;
        size_t bytes = (pages + 7) / 8;
        s->candidates = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* First pass includes historical mappings. Later writes re-arm only
         * the touched logical pages; an allocation failure disables reclaim. */
        if (s->candidates) memset(s->candidates, 0xff, bytes);
    }
#endif
    *out = &s->device;
    ESP_LOGI("mosaico_nand_storage", "NAND logical read cache: page=%u slots=%u psram_bytes=%u",
             (unsigned)s->page_size, (unsigned)s->count, (unsigned)(s->count * s->page_size));
    return ESP_OK;
}

esp_err_t storage_blockdev_reclaim(esp_blockdev_handle_t dev, const uint8_t *live,
                                   uint32_t pages, uint32_t *examined, uint32_t *discarded) {
    if (!dev || !live || !examined || !discarded) return ESP_ERR_INVALID_ARG;
    *examined = *discarded = 0;
#if CONFIG_MOSAICO_NAND_STORAGE_RECLAIM
    storage_blockdev_t *s = dev->ctx;
    if (!s->candidates || !s->source->ops->ioctl || !s->source->ops->sync)
        return ESP_ERR_NOT_SUPPORTED;
    if (!pages || pages > s->candidate_pages ||
        (uint64_t)pages * s->page_size != dev->geometry.disk_size)
        return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    int64_t slice = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    while (*examined < pages) {
        uint32_t p = *examined;
        ++*examined;
        const uint8_t bit = 1U << (p % 8);
        if ((s->candidates[p / 8] & bit) && !(live[p / 8] & bit)) {
            if (!*discarded) invalidate(s);
            esp_blockdev_cmd_arg_erase_t range = {
                .start_addr = (uint64_t)p * s->page_size, .erase_len = s->page_size};
            err = mutation_result(s->source->ops->ioctl(s->source, ESP_BLOCKDEV_CMD_MARK_DELETED, &range));
            if (err != ESP_OK) break;
            s->candidates[p / 8] &= (uint8_t)~bit;
            ++*discarded;
        }
        /* Finish the complete candidate sweep: a fixed per-job quota can fall
         * behind a large download and reproduce the same aging problem. Yield
         * without releasing the FS/device locks or invalidating the snapshot. */
        if (esp_timer_get_time() - slice >= 20000) {
#ifdef ESP_PLATFORM
            vTaskDelay(1);
#endif
            slice = esp_timer_get_time();
        }
    }
    /* Persist completed trims even when a later operation failed. Logical
     * roots were already durable; interrupted maintenance only leaks space. */
    if (*discarded) {
        esp_err_t sync = mutation_result(s->source->ops->sync(s->source));
        if (err == ESP_OK) err = sync;
    }
    xSemaphoreGive(s->lock);
    return err;
#else
    (void)pages;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
