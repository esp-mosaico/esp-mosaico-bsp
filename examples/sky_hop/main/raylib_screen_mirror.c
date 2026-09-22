// SPDX-License-Identifier: Apache-2.0
#include "raylib_screen_mirror.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "mosaico_game.h"
#include "mosaico_raylib_port.h"

#define FRAME_BYTES ((size_t)MOSAICO_GAME_WIDTH*MOSAICO_GAME_HEIGHT*2U)
#define POINTER_SERVICE_ID 0x1001U
#define POINTER_METHOD_ID 1U
#define POINTER_MESSAGE_SIZE 12U
static uint16_t *s_capture;

static int16_t read_le_i16(const uint8_t *value)
{ return (int16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8)); }

static esp_err_t pointer_rpc(const esp_iris_rpc_request_t *request,
    uint8_t *response, size_t capacity, size_t *response_size, void *ctx)
{
    (void)ctx;
    if (!request || request->payload_size != POINTER_MESSAGE_SIZE || !response ||
        capacity < POINTER_MESSAGE_SIZE || !response_size) return ESP_ERR_INVALID_SIZE;
    memcpy(response, request->payload, POINTER_MESSAGE_SIZE);
    int32_t x = read_le_i16(request->payload + 2), y = read_le_i16(request->payload + 4);
    if (x < 0) x = 0; else if (x >= 480) x = 479;
    if (y < 0) y = 0; else if (y >= 480) y = 479;
    mosaico_device_event_t event = {.type=MOSAICO_DEVICE_EVENT_POINTER, .x=x, .y=y,
        .pressed=request->payload[0] != 2, .timestamp_us=0};
    (void)MosaicoGamePostDeviceEvent(&event);
    *response_size = POINTER_MESSAGE_SIZE;
    return ESP_OK;
}

static esp_err_t screen_begin(const esp_iris_media_desc_t *requested,
    esp_iris_media_desc_t *actual, uint32_t *total_size, void *ctx)
{
    (void)requested; (void)ctx;
    if (!actual || !total_size || s_capture) return ESP_ERR_INVALID_STATE;
    s_capture = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_capture) return ESP_ERR_NO_MEM;
    esp_err_t err = mosaico_raylib_port_copy_latest(s_capture, 480U * 480U);
    if (err != ESP_OK) { heap_caps_free(s_capture); s_capture = NULL; return err; }
    *actual = (esp_iris_media_desc_t){.width=480, .height=480, .stride=960,
        .format=ESP_IRIS_PIXEL_FORMAT_RGB565};
    *total_size = FRAME_BYTES;
    return ESP_OK;
}

static esp_err_t screen_read(uint32_t offset, uint8_t *out, size_t capacity,
    size_t *out_size, void *ctx)
{
    (void)ctx;
    if (!s_capture || !out || !out_size || !capacity || offset >= FRAME_BYTES)
        return ESP_ERR_INVALID_ARG;
    size_t count = FRAME_BYTES - offset;
    if (count > capacity) count = capacity;
    memcpy(out, (uint8_t *)s_capture + offset, count);
    *out_size = count;
    return ESP_OK;
}

static void screen_end(void *ctx)
{ (void)ctx; heap_caps_free(s_capture); s_capture = NULL; }

esp_err_t raylib_screen_mirror_register(void)
{
    const esp_iris_screen_backend_t backend = {
        .begin=screen_begin, .read=screen_read, .end=screen_end};
    esp_err_t err = esp_iris_screen_register(&backend);
    return err == ESP_OK ? esp_iris_rpc_register(POINTER_SERVICE_ID,
        POINTER_METHOD_ID, pointer_rpc, NULL) : err;
}
