/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_module_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_sensor_types.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "linux/videodev2.h"

static const char *TAG = "mosaico_camera";

#define CAMERA_MAX_BUFFER_COUNT        8U
#define CAMERA_PWDN_WAKE_DELAY_MS      20U
#define CAMERA_XCLK_STABILIZATION_MS   20U
#define CAMERA_SCCB_FREQ_HZ            100000U
#define OV3640_PID_1                   0x364CU
#define OV3640_PID_2                   0x3641U
#define SC101IOT_PID                   0xDA4AU
#define OV3640_REG_AWB_CTRL            0x332BU
#define OV3640_AWB_CTRL_MANUAL_BIT     0x08U
#define OV3640_REG_AEC_AGC_CTRL        0x3013U
#define OV3640_REG_AEC_CTRL            0x3012U
#define OV3640_REG_AEC_EXPO_H          0x3500U
#define OV3640_REG_AEC_EXPO_M          0x3501U
#define OV3640_REG_AEC_EXPO_L          0x3502U
#define OV3640_REG_GAIN_H              0x350AU
#define OV3640_REG_GAIN_L              0x350BU
#define OV3640_FLASH_MAX_EXPOSURE      0x0B00U
#define OV3640_FLASH_MAX_GAIN_H        0x07U
#define OV3640_FLASH_MAX_GAIN_L        0xFFU
#define OV3640_AEC_AGC_ENABLE_BITS     0x05U
#define OV3640_AEC_AGC_DISABLE_MASK    0xFAU

typedef struct {
    uint16_t reg;
    uint8_t value;
} ov3640_reg_t;

typedef struct {
    gpio_num_t data_io[8];
    gpio_num_t vsync_io;
    gpio_num_t de_io;
    gpio_num_t pclk_io;
    gpio_num_t xclk_io;
    gpio_num_t reset_io;
    gpio_num_t pwdn_io;
    gpio_num_t flash_io;
    uint32_t xclk_freq_hz;
    uint32_t xclk_stabilization_ms;
    uint32_t sccb_freq_hz;
} mosaico_camera_hw_config_t;

static const ov3640_reg_t s_ov3640_advanced_awb[] = {
    {0x3317, 0x04}, {0x3316, 0xF8}, {0x3312, 0x26}, {0x3314, 0x42},
    {0x3313, 0x2B}, {0x3315, 0x42}, {0x3310, 0xD0}, {0x3311, 0xBD},
    {0x330C, 0x18}, {0x330D, 0x18}, {0x330E, 0x56}, {0x330F, 0x5C},
    {0x330B, 0x1C}, {0x3306, 0x5C}, {0x3307, 0x11}, {0x3308, 0x25},
};

struct mosaico_camera_t {
    mosaico_camera_config_t config;
    mosaico_camera_info_t info;
    mosaico_module_lease_t lease;
    mosaico_camera_hw_config_t hardware;
    SemaphoreHandle_t lock;
    int fd;
    bool subboard_claimed;
    bool hardware_claimed;
    bool video_initialized;
    bool opened;
    bool buffers_queued;
    bool streaming;
    bool power_down;
    bool resume_stream_after_wake;
    bool tuning_applied;
    esp_cam_sensor_id_t sensor_id;
    void *buffers[CAMERA_MAX_BUFFER_COUNT];
    size_t buffer_lengths[CAMERA_MAX_BUFFER_COUNT];
    bool outstanding[CAMERA_MAX_BUFFER_COUNT];
};

static portMUX_TYPE s_hardware_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_hardware_claimed;
static bool s_usj_pad_was_enabled;
static bool s_usj_clock_was_enabled;
static uint32_t s_usj_intr_enable_mask;

static esp_err_t camera_flash_force_off(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(GPIO_NUM_34),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_34, 1), TAG, "preset camera flash off failed");
    return gpio_config(&config);
}

static esp_err_t camera_hardware_acquire(bsp_subboard_slot_t slot, mosaico_camera_hw_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config, ESP_ERR_INVALID_ARG, TAG, "camera hardware output is null");
    ESP_RETURN_ON_FALSE(slot == BSP_SUBBOARD_SLOT_LEFT, ESP_ERR_NOT_SUPPORTED, TAG, "camera subboard supports the left slot only");
    ESP_RETURN_ON_ERROR(bsp_subboard_init(), TAG, "initialize subboard resources failed");

    bsp_subboard_slot_config_t slot_config = {0};
    ESP_RETURN_ON_ERROR(bsp_subboard_get_slot_config(slot, &slot_config), TAG, "get camera slot config failed");
    ESP_RETURN_ON_ERROR(camera_flash_force_off(), TAG, "initialize camera flash GPIO failed");

    portENTER_CRITICAL(&s_hardware_lock);
    if (s_hardware_claimed) {
        portEXIT_CRITICAL(&s_hardware_lock);
        ESP_LOGE(TAG, "Camera hardware is already claimed");
        return ESP_ERR_INVALID_STATE;
    }
    s_hardware_claimed = true;
    s_usj_pad_was_enabled = usb_serial_jtag_ll_phy_is_pad_enabled();
    s_usj_clock_was_enabled = usb_serial_jtag_ll_module_is_enabled();
    s_usj_intr_enable_mask = s_usj_clock_was_enabled ? usb_serial_jtag_ll_get_intr_ena_status() : 0;
    portEXIT_CRITICAL(&s_hardware_lock);

    /* Camera D2 reuses the built-in USB Serial/JTAG pad. */
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_phy_enable_pad(false);
    usb_serial_jtag_ll_enable_bus_clock(false);

    *out_config = (mosaico_camera_hw_config_t) {
        .data_io = {
            bsp_subboard_map_gpio(slot, GPIO_NUM_16), bsp_subboard_map_gpio(slot, GPIO_NUM_15), GPIO_NUM_33,
            bsp_subboard_map_gpio(slot, GPIO_NUM_4), slot_config.address_select_gpio,
            bsp_subboard_map_gpio(slot, GPIO_NUM_12), bsp_subboard_map_gpio(slot, GPIO_NUM_18),
            bsp_subboard_map_gpio(slot, GPIO_NUM_13),
        },
        .vsync_io = bsp_subboard_map_gpio(slot, GPIO_NUM_55),
        .de_io = bsp_subboard_map_gpio(slot, GPIO_NUM_19),
        .pclk_io = bsp_subboard_map_gpio(slot, GPIO_NUM_17),
        .xclk_io = GPIO_NUM_NC,
        .reset_io = bsp_subboard_map_gpio(slot, GPIO_NUM_53),
        .pwdn_io = bsp_subboard_map_gpio(slot, GPIO_NUM_48),
        .flash_io = GPIO_NUM_34,
        .xclk_freq_hz = 0,
        .xclk_stabilization_ms = CAMERA_XCLK_STABILIZATION_MS,
        .sccb_freq_hz = CAMERA_SCCB_FREQ_HZ,
    };
    ESP_LOGI(TAG, "Camera hardware claimed on left slot");
    return ESP_OK;
}

static esp_err_t camera_hardware_release(bsp_subboard_slot_t slot)
{
    ESP_RETURN_ON_FALSE(slot == BSP_SUBBOARD_SLOT_LEFT, ESP_ERR_NOT_SUPPORTED, TAG, "camera subboard supports the left slot only");

    portENTER_CRITICAL(&s_hardware_lock);
    const bool claimed = s_hardware_claimed;
    const bool restore_pad = s_usj_pad_was_enabled;
    const bool restore_clock = s_usj_clock_was_enabled;
    const uint32_t restore_intr_mask = s_usj_intr_enable_mask;
    portEXIT_CRITICAL(&s_hardware_lock);
    if (!claimed) {
        return ESP_OK;
    }

    if (restore_clock) {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
    usb_serial_jtag_ll_phy_enable_pad(restore_pad);
    if (restore_clock && restore_intr_mask) {
        usb_serial_jtag_ll_ena_intr_mask(restore_intr_mask);
    }
    portENTER_CRITICAL(&s_hardware_lock);
    s_hardware_claimed = false;
    portEXIT_CRITICAL(&s_hardware_lock);
    ESP_LOGI(TAG, "Camera hardware released");
    return ESP_OK;
}

static esp_err_t camera_ioctl(int fd, unsigned long request, void *arg,
                              const char *operation)
{
    if (ioctl(fd, request, arg) == 0) {
        return ESP_OK;
    }
    /*
     * esp-video returns ESP_FAIL when DQBUF receives no completed element.
     * Its VFS layer maps that generic error to EPERM, so treat this specific
     * combination as a frame timeout rather than a permission failure.
     */
    esp_err_t ret = (errno == ETIMEDOUT || errno == EAGAIN ||
                     (request == VIDIOC_DQBUF && errno == EPERM))
                        ? ESP_ERR_TIMEOUT
                        : ESP_FAIL;
    ESP_LOGE(TAG, "%s failed: errno=%d (%s) ret=%s", operation, errno,
             strerror(errno), esp_err_to_name(ret));
    return ret;
}

static uint32_t count_outstanding_buffers(mosaico_camera_handle_t camera)
{
    uint32_t outstanding = 0;
    for (uint32_t i = 0; i < camera->info.buffer_count; ++i) {
        if (camera->outstanding[i]) {
            ++outstanding;
        }
    }
    return outstanding;
}

static void log_pipeline_state(mosaico_camera_handle_t camera,
                               const char *reason, esp_log_level_t level)
{
    const uint32_t outstanding = count_outstanding_buffers(camera);
    ESP_LOG_LEVEL(
        level, TAG,
        "%s: streaming=%d power_down=%d outstanding=%" PRIu32 "/%" PRIu32,
        reason, camera->streaming, camera->power_down, outstanding,
        camera->info.buffer_count);
}

static const char *camera_sensor_name(uint16_t pid)
{
    if (pid == OV3640_PID_1 || pid == OV3640_PID_2) {
        return "OV3640";
    }
    if (pid == SC101IOT_PID) {
        return "SC101IOT";
    }
    return "unknown";
}

static bool camera_is_ov3640(const mosaico_camera_handle_t camera)
{
    return camera && (camera->sensor_id.pid == OV3640_PID_1 || camera->sensor_id.pid == OV3640_PID_2);
}

static esp_err_t camera_get_sensor_id(int fd, esp_cam_sensor_id_t *sensor_id)
{
    ESP_RETURN_ON_FALSE(sensor_id, ESP_ERR_INVALID_ARG, TAG, "sensor ID output is null");
    struct v4l2_ext_control control = {
        .id = ESP_CAM_SENSOR_IOC_G_CHIP_ID,
        .size = sizeof(*sensor_id),
        .p_u8 = (uint8_t *)sensor_id,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL,
        .count = 1,
        .controls = &control,
    };
    return camera_ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls, "get camera sensor ID");
}

/* OV3640-only register controls. */
static esp_err_t sensor_register_access(int fd, uint32_t control_id,
                                        esp_cam_sensor_reg_val_t *reg_value)
{
    struct v4l2_ext_control control = {
        .id = control_id,
        .size = sizeof(*reg_value),
        .p_u8 = (uint8_t *)reg_value,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL,
        .count = 1,
        .controls = &control,
    };
    const unsigned long request =
        control_id == ESP_CAM_SENSOR_IOC_S_REG ? VIDIOC_S_EXT_CTRLS
                                               : VIDIOC_G_EXT_CTRLS;
    return camera_ioctl(fd, request, &controls, "OV3640 register access");
}

static esp_err_t sensor_write_reg(int fd, uint16_t reg, uint8_t value)
{
    esp_cam_sensor_reg_val_t reg_value = {
        .regaddr = reg,
        .value = value,
    };
    return sensor_register_access(fd, ESP_CAM_SENSOR_IOC_S_REG, &reg_value);
}

static esp_err_t sensor_read_reg(int fd, uint16_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(value, ESP_ERR_INVALID_ARG, TAG,
                        "sensor register output is null");
    esp_cam_sensor_reg_val_t reg_value = {
        .regaddr = reg,
    };
    ESP_RETURN_ON_ERROR(
        sensor_register_access(fd, ESP_CAM_SENSOR_IOC_G_REG, &reg_value), TAG,
        "read OV3640 register 0x%04X failed", reg);
    *value = reg_value.value;
    return ESP_OK;
}

static esp_err_t apply_module_tuning(int fd)
{
    for (size_t i = 0;
         i < sizeof(s_ov3640_advanced_awb) / sizeof(s_ov3640_advanced_awb[0]);
         ++i) {
        ESP_RETURN_ON_ERROR(
            sensor_write_reg(fd, s_ov3640_advanced_awb[i].reg,
                             s_ov3640_advanced_awb[i].value),
            TAG, "write OV3640 tuning register 0x%04X failed",
            s_ov3640_advanced_awb[i].reg);
    }

    uint8_t awb_control = 0;
    ESP_RETURN_ON_ERROR(sensor_read_reg(fd, OV3640_REG_AWB_CTRL, &awb_control),
                        TAG, "read OV3640 AWB control failed");
    ESP_RETURN_ON_ERROR(
        sensor_write_reg(fd, OV3640_REG_AWB_CTRL,
                         (uint8_t)(awb_control &
                                   ~OV3640_AWB_CTRL_MANUAL_BIT)),
        TAG, "enable OV3640 automatic white balance failed");
    ESP_LOGI(TAG, "OV3640 module tuning applied; advanced AWB enabled");
    return ESP_OK;
}

static void flash_gpio_set_off(mosaico_camera_handle_t camera)
{
    if (camera && camera->hardware.flash_io != GPIO_NUM_NC) {
        gpio_set_level(camera->hardware.flash_io, 1);
    }
}

static esp_err_t flash_gpio_set_on(mosaico_camera_handle_t camera)
{
    if (!camera || camera->hardware.flash_io == GPIO_NUM_NC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return gpio_set_level(camera->hardware.flash_io, 0) == ESP_OK ? ESP_OK
                                                                  : ESP_FAIL;
}

static esp_err_t configure_flash_idle(mosaico_camera_handle_t camera)
{
    flash_gpio_set_off(camera);
    return ESP_OK;
}

static esp_err_t discard_stream_frames(mosaico_camera_handle_t camera,
                                       uint32_t count);

static esp_err_t queue_all_buffers(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera->opened, ESP_ERR_INVALID_STATE, TAG, "camera is not open");
    if (camera->buffers_queued) {
        return ESP_OK;
    }
    for (uint32_t i = 0; i < camera->info.buffer_count; ++i) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        esp_err_t ret =
            camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
        if (ret != ESP_OK) {
            return ret;
        }
    }
    camera->buffers_queued = true;
    return ESP_OK;
}

static esp_err_t start_stream(mosaico_camera_handle_t camera)
{
    if (camera->streaming) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(camera->opened, ESP_ERR_INVALID_STATE, TAG, "camera is not open");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_ERROR(camera_ioctl(camera->fd, VIDIOC_STREAMON, &type,
                                     "VIDIOC_STREAMON"),
                        TAG, "start camera stream failed");
    camera->streaming = true;
    ESP_LOGI(TAG, "Camera stream started");
    return ESP_OK;
}

static esp_err_t stop_stream(mosaico_camera_handle_t camera)
{
    if (!camera->streaming || camera->fd < 0) {
        return ESP_OK;
    }
    if (count_outstanding_buffers(camera) != 0) {
        log_pipeline_state(camera, "Stop blocked by outstanding frame", ESP_LOG_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_err_t ret =
        camera_ioctl(camera->fd, VIDIOC_STREAMOFF, &type,
                     "VIDIOC_STREAMOFF");
    if (ret == ESP_OK) {
        camera->buffers_queued = false;
        camera->streaming = false;
        ESP_LOGI(TAG, "Camera stream stopped");
    }
    return ret;
}

static esp_err_t close_video_device(mosaico_camera_handle_t camera)
{
    if (!camera) {
        return ESP_OK;
    }

    esp_err_t ret = stop_stream(camera);
    if (ret != ESP_OK) {
        return ret;
    }
    if (camera->power_down) {
        ESP_RETURN_ON_ERROR(gpio_set_level(camera->hardware.pwdn_io, 0), TAG, "wake camera before close failed");
        vTaskDelay(pdMS_TO_TICKS(CAMERA_PWDN_WAKE_DELAY_MS));
        camera->power_down = false;
        camera->resume_stream_after_wake = false;
    }
    camera->opened = false;

    for (size_t i = 0; i < CAMERA_MAX_BUFFER_COUNT; ++i) {
        if (camera->buffers[i] && camera->buffers[i] != MAP_FAILED) {
            if (munmap(camera->buffers[i], camera->buffer_lengths[i]) != 0) {
                ESP_LOGE(TAG, "munmap buffer %u failed: errno=%d", (unsigned)i, errno);
                return ESP_FAIL;
            }
            camera->buffers[i] = NULL;
            camera->buffer_lengths[i] = 0;
        }
    }

    if (camera->fd >= 0) {
        if (camera->info.buffer_count > 0) {
            struct v4l2_requestbuffers request = {
                .count = 0,
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = V4L2_MEMORY_MMAP,
            };
            ESP_RETURN_ON_ERROR(camera_ioctl(camera->fd, VIDIOC_REQBUFS, &request, "release camera buffers"), TAG,
                                "release camera buffers failed");
        }
        if (close(camera->fd) != 0) {
            ESP_LOGE(TAG, "Close video device failed: errno=%d", errno);
            return ESP_FAIL;
        }
        camera->fd = -1;
    }

    const mosaico_module_mgr_slot_t slot = camera->info.slot;
    camera->info = (mosaico_camera_info_t) {
        .slot = slot,
    };
    camera->buffers_queued = false;
    camera->streaming = false;
    camera->power_down = false;
    camera->resume_stream_after_wake = false;
    camera->tuning_applied = false;
    memset(&camera->sensor_id, 0, sizeof(camera->sensor_id));
    memset(camera->outstanding, 0, sizeof(camera->outstanding));
    return ESP_OK;
}

static esp_err_t release_resources(mosaico_camera_handle_t camera)
{
    if (!camera) {
        return ESP_OK;
    }

    esp_err_t ret = close_video_device(camera);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Close camera during cleanup failed; context retained: %s", esp_err_to_name(ret));
        return ret;
    }

    if (camera->video_initialized) {
        ret = esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_DVP);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Deinitialize DVP video failed; context retained: %s", esp_err_to_name(ret));
            return ret;
        }
        camera->video_initialized = false;
    }

    if (camera->hardware_claimed) {
        ret = camera_hardware_release(BSP_SUBBOARD_SLOT_LEFT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Release camera hardware failed; context retained: %s", esp_err_to_name(ret));
            return ret;
        }
        camera->hardware_claimed = false;
    }

    if (camera->subboard_claimed) {
        ret = mosaico_module_mgr_release(&camera->lease);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Release camera module failed; context retained: %s", esp_err_to_name(ret));
            return ret;
        }
        camera->subboard_claimed = false;
        camera->lease = (mosaico_module_lease_t) {0};
    }
    return ESP_OK;
}

static esp_err_t initialize_video_device(mosaico_camera_handle_t camera)
{
    esp_cam_ctlr_dvp_pin_config_t pins = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {
            camera->hardware.data_io[0], camera->hardware.data_io[1],
            camera->hardware.data_io[2], camera->hardware.data_io[3],
            camera->hardware.data_io[4], camera->hardware.data_io[5],
            camera->hardware.data_io[6], camera->hardware.data_io[7],
        },
        .vsync_io = camera->hardware.vsync_io,
        .de_io = camera->hardware.de_io,
        .pclk_io = camera->hardware.pclk_io,
        .xclk_io = camera->hardware.xclk_io,
    };
    esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = bsp_subboard_get_i2c_bus(),
            .freq = camera->hardware.sccb_freq_hz,
        },
        .reset_pin = camera->hardware.reset_io,
        .pwdn_pin = camera->hardware.pwdn_io,
        .dvp_pin = pins,
        .xclk_freq = camera->hardware.xclk_freq_hz,
    };
    const esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };

    ESP_RETURN_ON_ERROR(
        esp_video_init_with_flags(&video_config, ESP_VIDEO_INIT_FLAGS_DVP),
        TAG, "initialize DVP video device failed");
    camera->video_initialized = true;

    ESP_RETURN_ON_ERROR(configure_flash_idle(camera), TAG, "configure flash GPIO idle failed");
    ESP_LOGI(TAG, "Camera video device registered at %s", ESP_VIDEO_DVP_DEVICE_NAME);
    return ESP_OK;
}

static esp_err_t open_video_device(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera->video_initialized, ESP_ERR_INVALID_STATE, TAG, "camera video device is not initialized");
    if (camera->opened) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(camera->fd < 0, ESP_ERR_INVALID_STATE, TAG, "previous camera open cleanup is incomplete");

    camera->fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    ESP_RETURN_ON_FALSE(camera->fd >= 0, ESP_ERR_NOT_FOUND, TAG,
                        "open %s failed errno=%d",
                        ESP_VIDEO_DVP_DEVICE_NAME, errno);
    ESP_RETURN_ON_ERROR(configure_flash_idle(camera), TAG,
                        "configure flash GPIO idle failed");
    ESP_RETURN_ON_ERROR(camera_get_sensor_id(camera->fd, &camera->sensor_id), TAG, "query camera sensor failed");
    ESP_RETURN_ON_FALSE(camera_is_ov3640(camera) || camera->sensor_id.pid == SC101IOT_PID, ESP_ERR_NOT_SUPPORTED, TAG,
                        "unsupported camera sensor pid=0x%04" PRIx16, camera->sensor_id.pid);

    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (camera->config.width == 0) {
        ESP_RETURN_ON_ERROR(camera_ioctl(camera->fd, VIDIOC_G_FMT, &format, "VIDIOC_G_FMT"), TAG,
                            "get camera default format failed");
    } else {
        format.fmt.pix.width = camera->config.width;
        format.fmt.pix.height = camera->config.height;
    }
    const uint32_t requested_width = format.fmt.pix.width;
    const uint32_t requested_height = format.fmt.pix.height;
    format.fmt.pix.pixelformat = camera->config.pixel_format;
    ESP_RETURN_ON_ERROR(
        camera_ioctl(camera->fd, VIDIOC_S_FMT, &format, "VIDIOC_S_FMT"), TAG,
        "set camera format failed");
    const uint32_t requested_format = camera->config.pixel_format;
    if (format.fmt.pix.width != requested_width || format.fmt.pix.height != requested_height ||
        format.fmt.pix.pixelformat != requested_format) {
        ESP_LOGE(TAG, "Camera format mismatch: requested=%" PRIu32 "x%" PRIu32 "/0x%08" PRIx32
                      " actual=%" PRIu32 "x%" PRIu32 "/0x%08" PRIx32,
                 requested_width, requested_height, requested_format, format.fmt.pix.width,
                 format.fmt.pix.height, format.fmt.pix.pixelformat);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(configure_flash_idle(camera), TAG,
                        "configure flash GPIO after format failed");

    camera->info.width = format.fmt.pix.width;
    camera->info.height = format.fmt.pix.height;
    camera->info.bytes_per_line = format.fmt.pix.bytesperline;
    camera->info.pixel_format = format.fmt.pix.pixelformat;
    camera->info.frame_buffer_size = format.fmt.pix.sizeimage;

    struct v4l2_streamparm stream_parameters = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };
    if (camera_ioctl(camera->fd, VIDIOC_G_PARM, &stream_parameters,
                     "VIDIOC_G_PARM") == ESP_OK) {
        const struct v4l2_fract *rate =
            &stream_parameters.parm.capture.timeperframe;
        if (rate->numerator != 0) {
            camera->info.frame_rate = rate->denominator / rate->numerator;
        }
    }

    struct timeval timeout = {
        .tv_sec = camera->config.frame_timeout_ms / 1000,
        .tv_usec = (camera->config.frame_timeout_ms % 1000) * 1000,
    };
    ESP_RETURN_ON_ERROR(
        camera_ioctl(camera->fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout,
                     "VIDIOC_S_DQBUF_TIMEOUT"),
        TAG, "set frame timeout failed");

    struct v4l2_requestbuffers request = {
        .count = camera->config.buffer_count,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_ERROR(
        camera_ioctl(camera->fd, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS"),
        TAG, "request camera buffers failed");
    ESP_RETURN_ON_FALSE(request.count > 0 &&
                            request.count <= CAMERA_MAX_BUFFER_COUNT,
                        ESP_FAIL, TAG, "invalid camera buffer count=%" PRIu32,
                        request.count);
    camera->info.buffer_count = (uint8_t)request.count;

    for (uint32_t i = 0; i < request.count; ++i) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_RETURN_ON_ERROR(
            camera_ioctl(camera->fd, VIDIOC_QUERYBUF, &buffer,
                         "VIDIOC_QUERYBUF"),
            TAG, "query camera buffer %" PRIu32 " failed", i);

        camera->buffers[i] =
            mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                 camera->fd, buffer.m.offset);
        ESP_RETURN_ON_FALSE(camera->buffers[i] != MAP_FAILED,
                            ESP_ERR_NO_MEM, TAG,
                            "mmap camera buffer %" PRIu32 " failed errno=%d",
                            i, errno);
        camera->buffer_lengths[i] = buffer.length;
        if (camera->info.frame_buffer_size == 0) {
            camera->info.frame_buffer_size = buffer.length;
        }
        ESP_RETURN_ON_ERROR(
            camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF"),
            TAG, "queue camera buffer %" PRIu32 " failed", i);
    }
    camera->buffers_queued = true;
    camera->opened = true;
    ESP_LOGI(TAG, "Camera opened: sensor=%s pid=0x%04" PRIx16 " format=%" PRIu32 "x%" PRIu32,
             camera_sensor_name(camera->sensor_id.pid), camera->sensor_id.pid, camera->info.width, camera->info.height);
    return ESP_OK;
}

esp_err_t mosaico_camera_new(const mosaico_camera_config_t *config,
                             mosaico_camera_handle_t *out_camera)
{
    ESP_RETURN_ON_FALSE(out_camera, ESP_ERR_INVALID_ARG, TAG,
                        "camera output handle is null");
    *out_camera = NULL;

    mosaico_camera_config_t active =
        config ? *config
               : (mosaico_camera_config_t)MOSAICO_CAMERA_DEFAULT_CONFIG();
    ESP_RETURN_ON_FALSE(
        ((active.width == 0 && active.height == 0) || (active.width > 0 && active.height > 0)) &&
            active.buffer_count >= 1 &&
            active.buffer_count <= CAMERA_MAX_BUFFER_COUNT &&
            active.frame_timeout_ms > 0 && active.discovery_timeout_ms > 0 &&
            (active.pixel_format == V4L2_PIX_FMT_UYVY || active.pixel_format == V4L2_PIX_FMT_RGB565 ||
             active.pixel_format == V4L2_PIX_FMT_JPEG) &&
            (active.slot == MOSAICO_MODULE_MGR_SLOT_AUTO ||
             (active.slot >= MOSAICO_MODULE_MGR_SLOT_LEFT && active.slot < MOSAICO_MODULE_MGR_SLOT_COUNT)),
        ESP_ERR_INVALID_ARG, TAG, "invalid camera configuration");
    ESP_RETURN_ON_FALSE(active.slot != MOSAICO_MODULE_MGR_SLOT_RIGHT,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "camera subboard supports the left slot only");

    ESP_RETURN_ON_ERROR(mosaico_module_mgr_init(NULL), TAG,
                        "initialize module manager failed");

    mosaico_camera_handle_t camera =
        heap_caps_calloc(1, sizeof(*camera),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_NO_MEM, TAG,
                        "allocate camera context failed");
    camera->config = active;
    camera->fd = -1;
    camera->info.slot = MOSAICO_MODULE_MGR_SLOT_LEFT;
    camera->lock = xSemaphoreCreateMutex();
    if (!camera->lock) {
        heap_caps_free(camera);
        ESP_LOGE(TAG, "Create camera mutex failed: %s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    const mosaico_module_mgr_claim_config_t claim_config = {
        .expected_type = MOSAICO_BOARD_TYPE_CAMERA,
        .slot = MOSAICO_MODULE_MGR_SLOT_LEFT,
        .timeout_ms = active.discovery_timeout_ms,
        .flags = MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO |
                 MOSAICO_MODULE_CLAIM_PROBE_WHILE_CLAIMED |
                 (active.allow_unidentified ? MOSAICO_MODULE_CLAIM_ALLOW_INVALID_DESCRIPTOR : 0),
    };
    esp_err_t ret = mosaico_module_mgr_claim(&claim_config, &camera->lease);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Claim camera subboard failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }
    camera->subboard_claimed = true;

    ret = camera_hardware_acquire(BSP_SUBBOARD_SLOT_LEFT, &camera->hardware);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Acquire camera hardware failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    camera->hardware_claimed = true;

    if (camera->hardware.xclk_stabilization_ms > 0) {
        ESP_LOGI(TAG,
                 "Waiting %" PRIu32
                 " ms for CameraBoard XCLK stabilization before SCCB",
                 camera->hardware.xclk_stabilization_ms);
        vTaskDelay(pdMS_TO_TICKS(camera->hardware.xclk_stabilization_ms));
    }

    ret = initialize_video_device(camera);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initialize camera failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ESP_LOGI(TAG, "Camera registered: slot=left path=%s", ESP_VIDEO_DVP_DEVICE_NAME);
    *out_camera = camera;
    return ESP_OK;

fail:
    const esp_err_t cleanup_ret = release_resources(camera);
    if (cleanup_ret == ESP_OK) {
        vSemaphoreDelete(camera->lock);
        heap_caps_free(camera);
    } else {
        *out_camera = camera;
        ESP_LOGE(TAG, "Camera initialization cleanup incomplete; context retained: %s", esp_err_to_name(cleanup_ret));
    }
    return ret;
}

esp_err_t mosaico_camera_open(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    esp_err_t ret = open_video_device(camera);
    if (ret != ESP_OK) {
        const esp_err_t close_ret = close_video_device(camera);
        if (close_ret != ESP_OK) {
            ESP_LOGE(TAG, "Rollback failed camera open: %s", esp_err_to_name(close_ret));
        }
    }
    xSemaphoreGive(camera->lock);
    return ret;
}

esp_err_t mosaico_camera_start_stream(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (camera->streaming) {
        xSemaphoreGive(camera->lock);
        return ESP_OK;
    }
    esp_err_t ret = camera->power_down ? ESP_ERR_INVALID_STATE : queue_all_buffers(camera);
    if (ret == ESP_OK) {
        ret = start_stream(camera);
    }
    if (ret == ESP_OK && camera->config.apply_module_tuning && camera_is_ov3640(camera) && !camera->tuning_applied) {
        ret = apply_module_tuning(camera->fd);
        camera->tuning_applied = ret == ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = configure_flash_idle(camera);
    }
    if (ret == ESP_OK) {
        ret = discard_stream_frames(camera, camera->info.buffer_count);
    }
    if (ret != ESP_OK) {
        const esp_err_t stop_ret = stop_stream(camera);
        if (stop_ret != ESP_OK) {
            ESP_LOGE(TAG, "Rollback failed camera start: %s", esp_err_to_name(stop_ret));
        }
    }
    xSemaphoreGive(camera->lock);
    return ret;
}

esp_err_t mosaico_camera_stop_stream(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    const esp_err_t ret = stop_stream(camera);
    xSemaphoreGive(camera->lock);
    return ret;
}

esp_err_t mosaico_camera_close(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    const esp_err_t ret = close_video_device(camera);
    xSemaphoreGive(camera->lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera closed; video device remains registered");
    }
    return ret;
}

esp_err_t mosaico_camera_get_info(mosaico_camera_handle_t camera,
                                  mosaico_camera_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(camera && out_info, ESP_ERR_INVALID_ARG, TAG,
                        "invalid camera info request");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    *out_info = camera->info;
    xSemaphoreGive(camera->lock);
    return ESP_OK;
}

esp_err_t mosaico_camera_get_pipeline_stats(
    mosaico_camera_handle_t camera,
    mosaico_camera_pipeline_stats_t *out_stats)
{
    ESP_RETURN_ON_FALSE(camera && out_stats, ESP_ERR_INVALID_ARG, TAG,
                        "invalid pipeline stats request");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    out_stats->buffer_count = camera->info.buffer_count;
    out_stats->outstanding_count = (uint8_t)count_outstanding_buffers(camera);
    out_stats->streaming = camera->streaming;
    out_stats->power_down = camera->power_down;
    xSemaphoreGive(camera->lock);
    return ESP_OK;
}

esp_err_t mosaico_camera_get_frame(mosaico_camera_handle_t camera,
                                   mosaico_camera_frame_t *out_frame)
{
    ESP_RETURN_ON_FALSE(camera && out_frame, ESP_ERR_INVALID_ARG, TAG,
                        "invalid get frame request");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (!camera->streaming) {
        xSemaphoreGive(camera->lock);
        ESP_LOGE(TAG, "Get frame while stream is stopped: %s",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }

    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    esp_err_t ret =
        camera_ioctl(camera->fd, VIDIOC_DQBUF, &buffer, "VIDIOC_DQBUF");
    if (ret != ESP_OK) {
        log_pipeline_state(camera, "VIDIOC_DQBUF failed", ESP_LOG_WARN);
        xSemaphoreGive(camera->lock);
        return ret;
    }
    if (buffer.index >= camera->info.buffer_count ||
        !camera->buffers[buffer.index] ||
        camera->outstanding[buffer.index]) {
        log_pipeline_state(camera, "Invalid dequeued buffer", ESP_LOG_ERROR);
        xSemaphoreGive(camera->lock);
        ESP_LOGE(TAG, "Invalid dequeued buffer index=%" PRIu32 ": %s",
                 buffer.index, esp_err_to_name(ESP_ERR_INVALID_RESPONSE));
        return ESP_ERR_INVALID_RESPONSE;
    }

    if ((buffer.flags & V4L2_BUF_FLAG_ERROR) || buffer.bytesused == 0) {
        const esp_err_t queue_ret = camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
        ESP_LOGD(TAG, "Discard invalid frame index=%" PRIu32 " flags=0x%08" PRIx32 " bytes=%" PRIu32,
                 buffer.index, buffer.flags, buffer.bytesused);
        if (queue_ret != ESP_OK) {
            ESP_LOGE(TAG, "Requeue invalid frame failed: %s", esp_err_to_name(queue_ret));
        }
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t frame_size = buffer.bytesused;
    if (frame_size > camera->buffer_lengths[buffer.index]) {
        const esp_err_t queue_ret = camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
        ESP_LOGE(TAG, "Frame size %zu exceeds buffer %" PRIu32 " length %zu", frame_size, buffer.index,
                 camera->buffer_lengths[buffer.index]);
        if (queue_ret != ESP_OK) {
            ESP_LOGE(TAG, "Requeue oversized frame failed: %s", esp_err_to_name(queue_ret));
        }
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_RESPONSE;
    }

    camera->outstanding[buffer.index] = true;
    *out_frame = (mosaico_camera_frame_t) {
        .data = camera->buffers[buffer.index],
        .size = frame_size,
        .index = buffer.index,
        .width = camera->info.width,
        .height = camera->info.height,
        .bytes_per_line = camera->info.bytes_per_line,
        .pixel_format = camera->info.pixel_format,
    };
    ESP_LOGD(TAG, "Dequeued frame index=%" PRIu32 " bytes=%" PRIu32
                  " outstanding=%" PRIu32 "/%" PRIu32,
             buffer.index, buffer.bytesused,
             count_outstanding_buffers(camera), camera->info.buffer_count);
    xSemaphoreGive(camera->lock);
    return ESP_OK;
}

esp_err_t mosaico_camera_return_frame(mosaico_camera_handle_t camera,
                                      const mosaico_camera_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(camera && frame, ESP_ERR_INVALID_ARG, TAG,
                        "invalid return frame request");
    ESP_RETURN_ON_FALSE(frame->index < camera->info.buffer_count,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid frame index=%" PRIu32, frame->index);

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (!camera->outstanding[frame->index]) {
        xSemaphoreGive(camera->lock);
        if (!camera->streaming || camera->power_down) {
            return ESP_OK;
        }
        log_pipeline_state(camera, "Return unowned frame", ESP_LOG_ERROR);
        ESP_LOGE(TAG, "Frame %" PRIu32 " is not owned by caller: %s",
                 frame->index, esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }

    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = frame->index,
    };
    esp_err_t ret =
        camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
    if (ret == ESP_OK) {
        camera->outstanding[frame->index] = false;
        ESP_LOGD(TAG, "Requeued frame index=%" PRIu32 " outstanding=%" PRIu32
                      "/%" PRIu32,
                 frame->index, count_outstanding_buffers(camera),
                 camera->info.buffer_count);
    } else {
        log_pipeline_state(camera, "VIDIOC_QBUF failed", ESP_LOG_WARN);
    }
    xSemaphoreGive(camera->lock);
    return ret;
}

static esp_err_t discard_stream_frames(mosaico_camera_handle_t camera,
                                       uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        esp_err_t ret = camera_ioctl(camera->fd, VIDIOC_DQBUF, &buffer, "VIDIOC_DQBUF");
        if (ret != ESP_OK) {
            return ret;
        }
        ESP_RETURN_ON_FALSE(buffer.index < camera->info.buffer_count, ESP_ERR_INVALID_RESPONSE, TAG,
                            "invalid discarded buffer index=%" PRIu32, buffer.index);
        ret = camera_ioctl(camera->fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t mosaico_camera_discard_frames(mosaico_camera_handle_t camera,
                                        uint32_t count)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG,
                        "camera handle is null");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    esp_err_t ret = camera->streaming ? discard_stream_frames(camera, count) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(camera->lock);
    return ret;
}

static esp_err_t camera_query_ctrl(int fd, uint32_t id, struct v4l2_queryctrl *out)
{
    struct v4l2_queryctrl query = {
        .id = id,
    };
    ESP_RETURN_ON_ERROR(
        camera_ioctl(fd, VIDIOC_QUERYCTRL, &query, "VIDIOC_QUERYCTRL"),
        TAG, "query control 0x%08" PRIx32 " failed", id);
    if (query.flags & V4L2_CTRL_FLAG_DISABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *out = query;
    return ESP_OK;
}

static esp_err_t camera_query_ctrl_optional(int fd, uint32_t id,
                                             struct v4l2_queryctrl *out)
{
    struct v4l2_queryctrl query = {
        .id = id,
    };
    if (ioctl(fd, VIDIOC_QUERYCTRL, &query) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (query.flags & V4L2_CTRL_FLAG_DISABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *out = query;
    return ESP_OK;
}

static esp_err_t sensor_read_exposure(int fd, uint32_t *exposure,
                                      uint8_t *exposure_reg_l)
{
    uint8_t expo_h = 0;
    uint8_t expo_m = 0;
    uint8_t expo_l = 0;

    ESP_RETURN_ON_ERROR(sensor_read_reg(fd, OV3640_REG_AEC_EXPO_H, &expo_h), TAG,
                        "read OV3640 exposure high byte failed");
    ESP_RETURN_ON_ERROR(sensor_read_reg(fd, OV3640_REG_AEC_EXPO_M, &expo_m), TAG,
                        "read OV3640 exposure mid byte failed");
    ESP_RETURN_ON_ERROR(sensor_read_reg(fd, OV3640_REG_AEC_EXPO_L, &expo_l), TAG,
                        "read OV3640 exposure low byte failed");

    *exposure =
        ((uint32_t)expo_h << 12) | ((uint32_t)expo_m << 4) | ((expo_l >> 4) & 0x0FU);
    if (exposure_reg_l) {
        *exposure_reg_l = expo_l;
    }
    return ESP_OK;
}

static esp_err_t sensor_write_exposure(int fd, uint32_t exposure,
                                       uint8_t exposure_reg_l)
{
    ESP_RETURN_ON_ERROR(
        sensor_write_reg(fd, OV3640_REG_AEC_EXPO_H,
                         (uint8_t)((exposure >> 12) & 0xFFU)),
        TAG, "write OV3640 exposure high byte failed");
    ESP_RETURN_ON_ERROR(
        sensor_write_reg(fd, OV3640_REG_AEC_EXPO_M,
                         (uint8_t)((exposure >> 4) & 0xFFU)),
        TAG, "write OV3640 exposure mid byte failed");
    ESP_RETURN_ON_ERROR(
        sensor_write_reg(
            fd, OV3640_REG_AEC_EXPO_L,
            (uint8_t)((exposure & 0x0FU) << 4) | (exposure_reg_l & 0x0FU)),
        TAG, "write OV3640 exposure low byte failed");
    return ESP_OK;
}

static esp_err_t camera_get_ctrl(int fd, uint32_t id, int32_t *out_value)
{
    struct v4l2_control control = {
        .id = id,
    };
    ESP_RETURN_ON_ERROR(
        camera_ioctl(fd, VIDIOC_G_CTRL, &control, "VIDIOC_G_CTRL"),
        TAG, "get control 0x%08" PRIx32 " failed", id);
    *out_value = control.value;
    return ESP_OK;
}

static esp_err_t camera_set_ctrl(int fd, uint32_t id, int32_t value)
{
    struct v4l2_control control = {
        .id = id,
        .value = value,
    };
    return camera_ioctl(fd, VIDIOC_S_CTRL, &control, "VIDIOC_S_CTRL");
}

static esp_err_t prepare_flash_exposure(mosaico_camera_handle_t camera,
                                        mosaico_camera_flash_state_t *state)
{
    struct v4l2_queryctrl query = {0};
    esp_err_t ret =
        camera_query_ctrl_optional(camera->fd, V4L2_CID_EXPOSURE, &query);
    if (ret == ESP_OK) {
        int32_t max_exposure = query.maximum;
        ret = camera_get_ctrl(camera->fd, V4L2_CID_EXPOSURE, &state->exposure);
        if (ret == ESP_OK) {
            state->exposure_via_v4l2 = true;
            state->exposure_adjusted = true;
            ret = camera_set_ctrl(camera->fd, V4L2_CID_EXPOSURE, max_exposure);
        }
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Flash capture exposure via V4L2: %" PRId32 " -> max=%" PRId32,
                     state->exposure, max_exposure);
        }
        return ret;
    }

    uint32_t sensor_exposure = 0;
    ret = sensor_read_exposure(camera->fd, &sensor_exposure,
                               &state->exposure_reg_l);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OV3640 has no V4L2 exposure; sensor exposure read failed");
        return ESP_OK;
    }

    state->exposure = (int32_t)sensor_exposure;
    state->exposure_adjusted = true;
    ret = sensor_write_exposure(camera->fd, OV3640_FLASH_MAX_EXPOSURE,
                                state->exposure_reg_l);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Flash capture exposure via sensor reg: 0x%04" PRIx32
                      " -> max=0x%04" PRIx32,
                 sensor_exposure, (uint32_t)OV3640_FLASH_MAX_EXPOSURE);
    } else {
        ESP_LOGW(TAG, "Raise OV3640 exposure for flash failed: %s",
                 esp_err_to_name(ret));
        const esp_err_t restore_ret = sensor_write_exposure(camera->fd, sensor_exposure, state->exposure_reg_l);
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Restore exposure after prepare failure failed: %s", esp_err_to_name(restore_ret));
        } else {
            state->exposure_adjusted = false;
        }
        ret = ESP_OK;
    }
    return ret;
}

static esp_err_t restore_flash_exposure(mosaico_camera_handle_t camera,
                                        const mosaico_camera_flash_state_t *state)
{
    if (!state->exposure_adjusted) {
        return ESP_OK;
    }

    if (state->exposure_via_v4l2) {
        return camera_set_ctrl(camera->fd, V4L2_CID_EXPOSURE, state->exposure);
    }

    return sensor_write_exposure(camera->fd, (uint32_t)state->exposure,
                                 state->exposure_reg_l);
}

static esp_err_t prepare_flash_gain(mosaico_camera_handle_t camera,
                                    mosaico_camera_flash_state_t *state)
{
    esp_err_t ret = sensor_read_reg(camera->fd, OV3640_REG_GAIN_H, &state->gain_h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = sensor_read_reg(camera->fd, OV3640_REG_GAIN_L, &state->gain_l);
    if (ret != ESP_OK) {
        return ret;
    }
    state->gain_adjusted = true;
    ret = sensor_write_reg(camera->fd, OV3640_REG_GAIN_H, OV3640_FLASH_MAX_GAIN_H);
    if (ret == ESP_OK) {
        ret = sensor_write_reg(camera->fd, OV3640_REG_GAIN_L,
                               OV3640_FLASH_MAX_GAIN_L);
    }
    return ret;
}

static esp_err_t restore_flash_gain(mosaico_camera_handle_t camera,
                                    const mosaico_camera_flash_state_t *state)
{
    if (!state->gain_adjusted) {
        return ESP_OK;
    }
    const esp_err_t high_ret = sensor_write_reg(camera->fd, OV3640_REG_GAIN_H, state->gain_h);
    const esp_err_t low_ret = sensor_write_reg(camera->fd, OV3640_REG_GAIN_L, state->gain_l);
    return high_ret != ESP_OK ? high_ret : low_ret;
}

static esp_err_t restore_flash_state_locked(mosaico_camera_handle_t camera,
                                            const mosaico_camera_flash_state_t *state,
                                            bool restore_aec)
{
    esp_err_t result = ESP_OK;
    esp_err_t ret = restore_flash_gain(camera, state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Restore flash gain failed: %s", esp_err_to_name(ret));
        result = ret;
    }
    ret = restore_flash_exposure(camera, state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Restore flash exposure failed: %s", esp_err_to_name(ret));
        if (result == ESP_OK) {
            result = ret;
        }
    }
    if (restore_aec) {
        ret = sensor_write_reg(camera->fd, OV3640_REG_AEC_AGC_CTRL, state->aec_agc_ctrl);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Restore AEC/AGC failed: %s", esp_err_to_name(ret));
            if (result == ESP_OK) {
                result = ret;
            }
        }
    }
    return result;
}

esp_err_t mosaico_camera_set_power_down(mosaico_camera_handle_t camera, bool sleep)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG,
                        "camera handle is null");
    if (camera->hardware.pwdn_io == GPIO_NUM_NC) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    esp_err_t ret = ESP_OK;

    if (sleep == camera->power_down) {
        xSemaphoreGive(camera->lock);
        return ESP_OK;
    }

    if (sleep) {
        const bool resume_stream = camera->streaming;
        camera->resume_stream_after_wake = false;
        ret = stop_stream(camera);
        if (ret == ESP_OK &&
            gpio_set_level(camera->hardware.pwdn_io, 1) != ESP_OK) {
            ret = ESP_FAIL;
        }
        if (ret == ESP_OK) {
            camera->resume_stream_after_wake = resume_stream;
        } else if (resume_stream) {
            esp_err_t rollback_ret = queue_all_buffers(camera);
            if (rollback_ret == ESP_OK) {
                rollback_ret = start_stream(camera);
            }
            if (rollback_ret != ESP_OK) {
                ESP_LOGE(TAG, "Restore stream after PWDN failure failed: %s", esp_err_to_name(rollback_ret));
            }
        }
    } else {
        if (gpio_set_level(camera->hardware.pwdn_io, 0) != ESP_OK) {
            ret = ESP_FAIL;
        } else {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PWDN_WAKE_DELAY_MS));
            if (camera->resume_stream_after_wake) {
                ret = queue_all_buffers(camera);
            }
            if (ret == ESP_OK && camera->resume_stream_after_wake) {
                ret = start_stream(camera);
            }
        }
    }

    if (ret == ESP_OK) {
        camera->power_down = sleep;
        if (!sleep) {
            camera->resume_stream_after_wake = false;
        }
        ESP_LOGI(TAG, "Camera sensor %s via PWDN GPIO%d",
                 sleep ? "sleep" : "normal", camera->hardware.pwdn_io);
    }
    xSemaphoreGive(camera->lock);
    return ret;
}

bool mosaico_camera_is_power_down(mosaico_camera_handle_t camera)
{
    if (!camera) {
        return false;
    }
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    const bool power_down = camera->power_down;
    xSemaphoreGive(camera->lock);
    return power_down;
}

esp_err_t mosaico_camera_get_exposure(mosaico_camera_handle_t camera,
                                      int32_t *out_value)
{
    ESP_RETURN_ON_FALSE(camera && out_value, ESP_ERR_INVALID_ARG, TAG,
                        "invalid exposure read request");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (!camera->opened) {
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_STATE;
    }
    struct v4l2_queryctrl query = {0};
    esp_err_t ret = camera_query_ctrl(camera->fd, V4L2_CID_EXPOSURE, &query);
    if (ret == ESP_OK) {
        ret = camera_get_ctrl(camera->fd, V4L2_CID_EXPOSURE, out_value);
    }
    xSemaphoreGive(camera->lock);
    return ret;
}

esp_err_t mosaico_camera_prepare_flash_capture(
    mosaico_camera_handle_t camera, mosaico_camera_flash_state_t *state)
{
    ESP_RETURN_ON_FALSE(camera && state, ESP_ERR_INVALID_ARG, TAG,
                        "invalid flash prepare request");

    memset(state, 0, sizeof(*state));
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (!camera->opened) {
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!camera_is_ov3640(camera)) {
        xSemaphoreGive(camera->lock);
        return ESP_OK;
    }

    bool restore_aec = false;
    esp_err_t ret = sensor_read_reg(camera->fd, OV3640_REG_AEC_CTRL, &state->aec_ctrl_3012);
    if (ret == ESP_OK) {
        ret = sensor_read_reg(camera->fd, OV3640_REG_AEC_AGC_CTRL,
                              &state->aec_agc_ctrl);
    }
    if (ret == ESP_OK) {
        restore_aec = true;
        ret = sensor_write_reg(
            camera->fd, OV3640_REG_AEC_AGC_CTRL,
            (uint8_t)(state->aec_agc_ctrl & OV3640_AEC_AGC_DISABLE_MASK));
    }

    if (ret == ESP_OK) {
        ret = prepare_flash_exposure(camera, state);
    }
    if (ret == ESP_OK) {
        ret = prepare_flash_gain(camera, state);
    }

    if (ret == ESP_OK) {
        state->prepared = true;
        ESP_LOGI(TAG,
                 "Flash capture prepared: AEC/AGC paused, exposure%s%s gain%s",
                 state->exposure_adjusted ? " max" : " unchanged",
                 state->exposure_adjusted
                     ? (state->exposure_via_v4l2 ? " (v4l2)" : " (reg)")
                     : "",
                 state->gain_adjusted ? " max" : " unchanged");
    } else {
        const esp_err_t rollback_ret = restore_flash_state_locked(camera, state, restore_aec);
        if (rollback_ret != ESP_OK) {
            ESP_LOGE(TAG, "Flash preparation rollback incomplete: %s", esp_err_to_name(rollback_ret));
        }
    }
    xSemaphoreGive(camera->lock);
    return ret;
}

esp_err_t mosaico_camera_restore_flash_capture(
    mosaico_camera_handle_t camera, const mosaico_camera_flash_state_t *state)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG,
                        "camera handle is null");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (!camera->opened) {
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_STATE;
    }
    flash_gpio_set_off(camera);
    esp_err_t ret = ESP_OK;
    if (state && state->prepared && camera_is_ov3640(camera)) {
        ret = restore_flash_state_locked(camera, state, true);
    }
    xSemaphoreGive(camera->lock);
    return ret;
}

esp_err_t mosaico_camera_flash_trigger(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG,
                        "camera handle is null");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    const esp_err_t ret = flash_gpio_set_on(camera);
    xSemaphoreGive(camera->lock);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Flash enabled on GPIO%d for capture",
                 camera->hardware.flash_io);
    }
    return ret;
}

esp_err_t mosaico_camera_flash_stop(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG,
                        "camera handle is null");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    flash_gpio_set_off(camera);
    xSemaphoreGive(camera->lock);
    ESP_LOGD(TAG, "Flash disabled on GPIO%d", camera->hardware.flash_io);
    return ESP_OK;
}

esp_err_t mosaico_camera_strobe_trigger(mosaico_camera_handle_t camera)
{
    return mosaico_camera_flash_trigger(camera);
}

esp_err_t mosaico_camera_strobe_stop(mosaico_camera_handle_t camera)
{
    return mosaico_camera_flash_stop(camera);
}

esp_err_t mosaico_camera_set_exposure(mosaico_camera_handle_t camera,
                                     int32_t value)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG,
                        "camera handle is null");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (!camera->opened) {
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_STATE;
    }
    struct v4l2_queryctrl query = {0};
    esp_err_t ret = camera_query_ctrl(camera->fd, V4L2_CID_EXPOSURE, &query);
    if (ret == ESP_OK) {
        if (value < query.minimum) {
            value = query.minimum;
        }
        if (value > query.maximum) {
            value = query.maximum;
        }
        if (query.step <= 0) {
            ESP_LOGE(TAG, "Invalid exposure step: %" PRId32, query.step);
            ret = ESP_ERR_INVALID_RESPONSE;
        } else {
            value = query.minimum + ((value - query.minimum) / query.step) * query.step;
            ret = camera_set_ctrl(camera->fd, V4L2_CID_EXPOSURE, value);
        }
    }
    xSemaphoreGive(camera->lock);
    return ret;
}

esp_err_t mosaico_camera_restart(mosaico_camera_handle_t camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG,
                        "camera handle is null");
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (camera->power_down) {
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t i = 0; i < camera->info.buffer_count; ++i) {
        if (camera->outstanding[i]) {
            log_pipeline_state(camera, "Restart blocked by outstanding frame",
                               ESP_LOG_ERROR);
            xSemaphoreGive(camera->lock);
            ESP_LOGE(TAG,
                     "Return all frames before restarting the stream: %s",
                     esp_err_to_name(ESP_ERR_INVALID_STATE));
            return ESP_ERR_INVALID_STATE;
        }
    }
    ESP_LOGW(TAG, "Restarting camera stream");
    esp_err_t ret = stop_stream(camera);
    if (ret != ESP_OK) {
        xSemaphoreGive(camera->lock);
        return ret;
    }

    ret = queue_all_buffers(camera);
    if (ret != ESP_OK) {
        xSemaphoreGive(camera->lock);
        return ret;
    }
    ret = start_stream(camera);
    xSemaphoreGive(camera->lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera stream restarted");
    }
    return ret;
}

esp_err_t mosaico_camera_del(mosaico_camera_handle_t camera)
{
    if (!camera) {
        return ESP_OK;
    }
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    const esp_err_t ret = release_resources(camera);
    xSemaphoreGive(camera->lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera delete deferred because resources remain: %s", esp_err_to_name(ret));
        return ret;
    }
    vSemaphoreDelete(camera->lock);
    heap_caps_free(camera);
    ESP_LOGI(TAG, "Camera deleted and resources released");
    return ESP_OK;
}
