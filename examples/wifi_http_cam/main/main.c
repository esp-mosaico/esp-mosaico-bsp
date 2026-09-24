/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief SoftAP + HTTP MJPEG/snapshot from an ESP-Mosaico CameraBoard.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "driver/jpeg_encode.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mosaico_module_camera.h"
#include "nvs_flash.h"

#define WIFI_AP_SSID               "mosaico-cam"
#define WIFI_AP_CHANNEL            6
#define WIFI_AP_MAX_CONN           4

#define CAMERA_RETRY_DELAY_MS      500
#define JPEG_QUALITY               80
#define JPEG_TIMEOUT_MS            1000
#define STREAM_BOUNDARY            "mosaicoframe"

static const char *TAG = "wifi_http_cam";

static const char *INDEX_HTML =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Mosaico Cam</title>"
    "<style>body{margin:0;background:#111;color:#eee;font-family:sans-serif;text-align:center}"
    "img{max-width:100%;height:auto}</style></head><body>"
    "<h1>Mosaico Cam</h1>"
    "<p><a href=\"/jpg\">snapshot</a> · <a href=\"/stream\">MJPEG stream</a></p>"
    "<img src=\"/stream\" alt=\"stream\"></body></html>";

typedef struct {
    mosaico_camera_handle_t camera;
    jpeg_encoder_handle_t jpeg_encoder;
    uint8_t *jpeg_buffer;
    size_t jpeg_capacity;
    SemaphoreHandle_t frame_lock;
    httpd_handle_t http;
} app_context_t;

static app_context_t s_app;

static esp_err_t wifi_softap_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, WIFI_AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "ap config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "SoftAP SSID=%s  open http://192.168.4.1/", WIFI_AP_SSID);
    return ESP_OK;
}

static esp_err_t camera_wait_and_open(void)
{
    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.pixel_format = V4L2_PIX_FMT_UYVY;
    config.allow_unidentified = true;
    while (true) {
        esp_err_t ret = mosaico_camera_new(&config, &s_app.camera);
        if (ret == ESP_OK) {
            ret = mosaico_camera_open(s_app.camera);
        }
        if (ret == ESP_OK) {
            ret = mosaico_camera_start_stream(s_app.camera);
        }
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (s_app.camera) {
            const esp_err_t cleanup_ret = mosaico_camera_del(s_app.camera);
            if (cleanup_ret == ESP_OK) {
                s_app.camera = NULL;
            } else {
                return cleanup_ret;
            }
        }
        ESP_LOGW(TAG, "Open camera failed, retrying: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_DELAY_MS));
    }
}

static esp_err_t app_buffers_init(void)
{
    s_app.frame_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_app.frame_lock, ESP_ERR_NO_MEM, TAG, "frame lock");

    mosaico_camera_info_t info = {0};
    ESP_RETURN_ON_ERROR(mosaico_camera_get_info(s_app.camera, &info), TAG, "camera info");
    const jpeg_encode_memory_alloc_cfg_t memory = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    s_app.jpeg_buffer = jpeg_alloc_encoder_mem(info.frame_buffer_size, &memory, &s_app.jpeg_capacity);
    ESP_RETURN_ON_FALSE(s_app.jpeg_buffer, ESP_ERR_NO_MEM, TAG, "JPEG output buffer");

    const jpeg_encode_engine_cfg_t engine_config = {
        .timeout_ms = JPEG_TIMEOUT_MS,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&engine_config, &s_app.jpeg_encoder), TAG, "JPEG encoder");
    return ESP_OK;
}

static esp_err_t capture_jpeg(size_t *jpeg_size)
{
    mosaico_camera_frame_t frame = {0};
    ESP_RETURN_ON_ERROR(mosaico_camera_get_frame(s_app.camera, &frame), TAG, "get frame");

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    uint32_t encoded_size = 0;
    if (frame.pixel_format == V4L2_PIX_FMT_UYVY && frame.size) {
        const jpeg_encode_cfg_t config = {
            .height = frame.height,
            .width = frame.width,
            .src_type = JPEG_ENCODE_IN_FORMAT_YUV422,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
            .image_quality = JPEG_QUALITY,
        };
        err = jpeg_encoder_process(s_app.jpeg_encoder, &config, frame.data, frame.size, s_app.jpeg_buffer,
                                   s_app.jpeg_capacity, &encoded_size);
    }
    const esp_err_t return_err = mosaico_camera_return_frame(s_app.camera, &frame);
    ESP_RETURN_ON_ERROR(err, TAG, "JPEG encode");
    ESP_RETURN_ON_ERROR(return_err, TAG, "return frame");
    *jpeg_size = encoded_size;
    return ESP_OK;
}

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_jpg(httpd_req_t *req)
{
    if (xSemaphoreTake(s_app.frame_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_FAIL;
    }

    size_t jpeg_size = 0;
    esp_err_t err = capture_jpeg(&jpeg_size);
    if (err != ESP_OK) {
        xSemaphoreGive(s_app.frame_lock);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, (const char *)s_app.jpeg_buffer, jpeg_size);
    xSemaphoreGive(s_app.frame_lock);
    return err;
}

static esp_err_t handle_stream(httpd_req_t *req)
{
    char part_hdr[128];
    esp_err_t err = httpd_resp_set_type(
        req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    if (err != ESP_OK) {
        return err;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    while (true) {
        if (xSemaphoreTake(s_app.frame_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
            continue;
        }

        size_t jpeg_size = 0;
        err = capture_jpeg(&jpeg_size);
        if (err != ESP_OK) {
            xSemaphoreGive(s_app.frame_lock);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                               "\r\n--" STREAM_BOUNDARY "\r\n"
                               "Content-Type: image/jpeg\r\n"
                               "Content-Length: %d\r\n\r\n",
                               (int)jpeg_size);
        if (httpd_resp_send_chunk(req, part_hdr, hdr_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)s_app.jpeg_buffer, jpeg_size) != ESP_OK) {
            xSemaphoreGive(s_app.frame_lock);
            ESP_LOGW(TAG, "stream client disconnected");
            break;
        }
        xSemaphoreGive(s_app.frame_lock);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;
    config.stack_size = 8192;

    ESP_RETURN_ON_ERROR(httpd_start(&s_app.http, &config), TAG, "httpd start");

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_index,
    };
    const httpd_uri_t jpg_uri = {
        .uri = "/jpg",
        .method = HTTP_GET,
        .handler = handle_jpg,
    };
    const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = handle_stream,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.http, &index_uri), TAG, "/");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.http, &jpg_uri), TAG, "/jpg");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_app.http, &stream_uri), TAG, "/stream");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "SoftAP HTTP camera");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wifi_softap_start());
    ESP_ERROR_CHECK(camera_wait_and_open());
    ESP_ERROR_CHECK(app_buffers_init());
    size_t jpeg_size = 0;
    ESP_ERROR_CHECK(capture_jpeg(&jpeg_size));
    ESP_LOGI(TAG, "JPEG encoder ready: %u bytes", (unsigned)jpeg_size);
    ESP_ERROR_CHECK(http_server_start());
    ESP_LOGI(TAG, "Ready: connect to %s then open http://192.168.4.1/", WIFI_AP_SSID);
}
