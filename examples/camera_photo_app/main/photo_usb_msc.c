/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_CAMERA_PHOTO_USB_MSC

#include "photo_usb_msc.h"
#include "photo_store.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"

static const char *TAG = "photo_usb_msc";

#define USB_MSC_BASE_PATH   "/usb"
#define USB_MSC_PHOTOS_DIR  USB_MSC_BASE_PATH "/DCIM"

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_storage_hdl = NULL;
static SemaphoreHandle_t s_lock = NULL;
static bool s_initialized = false;

static bool storage_mounted_to_app(void)
{
    if (!s_storage_hdl) {
        return false;
    }

    tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    if (tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mount_point) != ESP_OK) {
        return false;
    }
    return mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP;
}

static esp_err_t ensure_photos_dir(void)
{
    struct stat st = {0};
    if (stat(USB_MSC_PHOTOS_DIR, &st) == 0) {
        return ESP_OK;
    }
    if (mkdir(USB_MSC_PHOTOS_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: errno=%d", USB_MSC_PHOTOS_DIR, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_jpeg_file(const char *filename, const uint8_t *jpeg_data,
                                 size_t jpeg_size)
{
    char path[64];
    const int path_len = snprintf(path, sizeof(path), "%s/%s", USB_MSC_PHOTOS_DIR,
                                  filename);
    if (path_len <= 0 || path_len >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "open %s for write failed: errno=%d", path, errno);
        return ESP_FAIL;
    }

    const size_t written = fwrite(jpeg_data, 1, jpeg_size, file);
    fclose(file);
    if (written != jpeg_size) {
        ESP_LOGE(TAG, "write %s failed: wrote %u/%u bytes", path,
                 (unsigned)written, (unsigned)jpeg_size);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool catalog_contains_filename(const char *filename)
{
    const uint32_t count = photo_store_get_count();
    photo_store_info_t info = {0};

    for (uint32_t i = 0; i < count; ++i) {
        if (photo_store_get_info(i, &info) != ESP_OK) {
            continue;
        }
        if (strcmp(info.filename, filename) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t remove_stale_jpegs(void)
{
    DIR *dir = opendir(USB_MSC_PHOTOS_DIR);
    if (!dir) {
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) {
            continue;
        }
        const size_t name_len = strlen(entry->d_name);
        if (name_len < 5 ||
            strcasecmp(entry->d_name + name_len - 4, ".jpg") != 0) {
            continue;
        }
        if (catalog_contains_filename(entry->d_name)) {
            continue;
        }

        char path[64];
        snprintf(path, sizeof(path), "%s/%s", USB_MSC_PHOTOS_DIR, entry->d_name);
        if (remove(path) != 0) {
            ESP_LOGW(TAG, "remove stale file %s failed: errno=%d", path, errno);
        } else {
            ESP_LOGI(TAG, "Removed stale USB export %s", entry->d_name);
        }
    }

    closedir(dir);
    return ESP_OK;
}

static esp_err_t export_store_index(uint32_t index, uint8_t *jpeg_buffer)
{
    photo_store_info_t info = {0};
    ESP_RETURN_ON_ERROR(photo_store_get_info(index, &info), TAG,
                        "get photo info failed");

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", USB_MSC_PHOTOS_DIR, info.filename);

    struct stat st = {0};
    if (stat(path, &st) == 0 && (size_t)st.st_size == info.size_bytes) {
        return ESP_OK;
    }

    size_t loaded_size = 0;
    ESP_RETURN_ON_ERROR(
        photo_store_load_jpeg(index, jpeg_buffer, PHOTO_USB_MSC_MAX_JPEG_SIZE,
                              &loaded_size),
        TAG, "load %s failed", info.filename);

    return write_jpeg_file(info.filename, jpeg_buffer, loaded_size);
}

static void storage_event_cb(tinyusb_msc_storage_handle_t handle,
                             tinyusb_msc_event_t *event, void *arg)
{
    (void)arg;

    if (event->id != TINYUSB_MSC_EVENT_MOUNT_START || handle != s_storage_hdl) {
        return;
    }

    tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    if (tinyusb_msc_get_storage_mount_point(handle, &mount_point) != ESP_OK) {
        return;
    }

    if (mount_point != TINYUSB_MSC_STORAGE_MOUNT_APP) {
        return;
    }

    ESP_LOGI(TAG, "USB host connecting; syncing photos before MSC expose");
    const esp_err_t ret = photo_usb_msc_sync_from_store();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Pre-USB photo sync failed: %s", esp_err_to_name(ret));
    }
}

static esp_err_t init_wl_partition(wl_handle_t *out_wl_handle)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    ESP_RETURN_ON_FALSE(partition, ESP_ERR_NOT_FOUND, TAG,
                        "FAT storage partition not found");

    ESP_LOGI(TAG, "Mounting USB storage partition at 0x%" PRIx32 ", size=%" PRIu32,
             partition->address, partition->size);
    return wl_mount(partition, out_wl_handle);
}

esp_err_t photo_usb_msc_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "USB MSC already initialized");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create USB MSC lock failed");

    const tinyusb_msc_driver_config_t driver_cfg = {
        .callback = storage_event_cb,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver_cfg), TAG,
                        "install MSC driver failed");
    ESP_RETURN_ON_ERROR(init_wl_partition(&s_wl_handle), TAG,
                        "init wear levelling failed");

    static char base_path[] = USB_MSC_BASE_PATH;
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    const tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .medium.wl_handle = s_wl_handle,
        .fat_fs = {
            .base_path = base_path,
            .config = mount_config,
            .do_not_format = false,
        },
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_storage_hdl),
                        TAG, "create MSC storage failed");

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG,
                        "install TinyUSB driver failed");

    ESP_RETURN_ON_ERROR(ensure_photos_dir(), TAG, "create DCIM directory failed");

    const esp_err_t sync_ret = photo_usb_msc_sync_from_store();
    if (sync_ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial photo sync failed: %s", esp_err_to_name(sync_ret));
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "USB High-Speed MSC ready; connect to a PC to browse %s",
             USB_MSC_PHOTOS_DIR);
    return ESP_OK;
}

bool photo_usb_msc_is_host_mounted(void)
{
    if (!s_initialized || !s_storage_hdl) {
        return false;
    }

    tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;
    if (tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mount_point) != ESP_OK) {
        return false;
    }
    return mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB;
}

esp_err_t photo_usb_msc_export_jpeg(const char *filename, const uint8_t *jpeg_data,
                                    size_t jpeg_size)
{
    ESP_RETURN_ON_FALSE(filename && jpeg_data && jpeg_size > 0, ESP_ERR_INVALID_ARG,
                        TAG, "invalid export request");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "USB MSC is not initialized");
    if (photo_usb_msc_is_host_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(storage_mounted_to_app(), ESP_ERR_INVALID_STATE, TAG,
                        "USB storage is not mounted to app");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ensure_photos_dir();
    if (ret == ESP_OK) {
        ret = write_jpeg_file(filename, jpeg_data, jpeg_size);
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t photo_usb_msc_remove_jpeg(const char *filename)
{
    ESP_RETURN_ON_FALSE(filename, ESP_ERR_INVALID_ARG, TAG, "filename is null");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "USB MSC is not initialized");
    if (photo_usb_msc_is_host_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(storage_mounted_to_app(), ESP_ERR_INVALID_STATE, TAG,
                        "USB storage is not mounted to app");

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", USB_MSC_PHOTOS_DIR, filename);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t ret = (remove(path) == 0 || errno == ENOENT) ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t photo_usb_msc_sync_from_store(void)
{
    ESP_RETURN_ON_FALSE(s_storage_hdl, ESP_ERR_INVALID_STATE, TAG,
                        "USB MSC storage is not ready");
    if (photo_usb_msc_is_host_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(storage_mounted_to_app(), ESP_ERR_INVALID_STATE, TAG,
                        "USB storage is not mounted to app");

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t ret = ensure_photos_dir();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ret;
    }

    uint8_t *jpeg_buffer =
        heap_caps_malloc(PHOTO_USB_MSC_MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buffer) {
        jpeg_buffer = malloc(PHOTO_USB_MSC_MAX_JPEG_SIZE);
    }
    if (!jpeg_buffer) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    const uint32_t count = photo_store_get_count();
    esp_err_t first_err = ESP_OK;
    for (uint32_t i = 0; i < count; ++i) {
        const esp_err_t export_ret = export_store_index(i, jpeg_buffer);
        if (export_ret != ESP_OK && first_err == ESP_OK) {
            first_err = export_ret;
            ESP_LOGW(TAG, "Export photo index=%" PRIu32 " failed: %s", i,
                     esp_err_to_name(export_ret));
        }
    }

    if (first_err == ESP_OK) {
        first_err = remove_stale_jpegs();
    }

    free(jpeg_buffer);
    xSemaphoreGive(s_lock);

    if (first_err == ESP_OK) {
        ESP_LOGI(TAG, "Synced %u photo(s) to USB storage", (unsigned)count);
    }
    return first_err;
}

#endif /* CONFIG_CAMERA_PHOTO_USB_MSC */
