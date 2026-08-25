/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/usb_console.h"
#include "sdkconfig.h"

#if CONFIG_BSP_USB_CONSOLE

#include <stdio.h>
#include <stdatomic.h>

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"

static const char *TAG = "bsp_usb_console";
static bool s_initialized;

#define BSP_USB_SERIAL_LENGTH 12

static const char s_usb_langid[] = {0x09, 0x04};
static char s_usb_serial[BSP_USB_SERIAL_LENGTH + 1];
static const char *s_usb_string_descriptors[] = {
    s_usb_langid,
    CONFIG_TINYUSB_DESC_MANUFACTURER_STRING,
    CONFIG_TINYUSB_DESC_PRODUCT_STRING,
    s_usb_serial,
    CONFIG_TINYUSB_DESC_CDC_STRING,
};

#if CONFIG_BSP_USB_AUTO_DOWNLOAD

/*
 * esptool and idf_monitor select the USB-Serial/JTAG reset strategy when they
 * see Espressif's USB-Serial/JTAG VID/PID. This device implements the matching
 * CDC reset protocol in software; it does not implement a JTAG interface.
 */
#define BSP_USB_SERIAL_JTAG_PID 0x1001
#define BSP_USB_RESTART_GRACE_US (50 * 1000)
#define BSP_USB_RESTART_FALLBACK_US (500 * 1000)

static const tusb_desc_device_t s_usb_device_descriptor = {
    .bLength = sizeof(s_usb_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = BSP_USB_SERIAL_JTAG_PID,
    .bcdDevice = CONFIG_TINYUSB_DESC_BCD_DEVICE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

typedef enum {
    BSP_USB_REBOOT_NONE,
    BSP_USB_REBOOT_NORMAL,
    BSP_USB_REBOOT_BOOTLOADER,
} bsp_usb_reboot_t;

static atomic_int s_reboot_mode = ATOMIC_VAR_INIT(BSP_USB_REBOOT_NONE);
static atomic_bool s_download_mode = ATOMIC_VAR_INIT(false);
static esp_timer_handle_t s_restart_timer;

static void usb_console_before_restart(void)
{
    const bsp_usb_reboot_t mode = atomic_load_explicit(&s_reboot_mode, memory_order_acquire);
    if (mode == BSP_USB_REBOOT_BOOTLOADER) {
        REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    } else if (mode == BSP_USB_REBOOT_NORMAL) {
        REG_CLR_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    }
}

static void restart_timer_callback(void *arg)
{
    (void)arg;
    esp_restart();
}

static void cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    const bool dtr = event->line_state_changed_data.dtr;
    const bool rts = event->line_state_changed_data.rts;

    /*
     * Match the USB-Serial/JTAG CDC state table exactly:
     *
     *   RTS DTR  action
     *    0   0   clear download-mode latch
     *    0   1   set download-mode latch
     *    1   0   reset using the latched mode
     *    1   1   no action
     *
     * esptool's USBJTAGSerialReset generates these effective phases:
     *
     *   (0,0) --100 ms-- (0,1) --100 ms-- (1,1) -> (1,0)
     *           --100 ms-- (0,0)
     *
     * Its RTS helper also re-writes the current DTR state for Windows
     * usbser.sys. Repeated states are therefore intentional. The reboot-mode
     * compare/exchange below accepts only the first (1,0) event.
     */
    if (!rts) {
        atomic_store_explicit(&s_download_mode, dtr, memory_order_release);

        /*
         * (0,0) after a pending (1,0) is the end of esptool's reset
         * transaction. Restart only after the host has released both lines;
         * otherwise the device can disappear while the host is still issuing
         * control transfers. The fallback timer remains useful when this
         * final state is lost because of a host-side disconnect.
         */
        if (!dtr &&
            atomic_load_explicit(&s_reboot_mode, memory_order_acquire) != BSP_USB_REBOOT_NONE) {
            (void)esp_timer_restart(s_restart_timer, BSP_USB_RESTART_GRACE_US);
        }
        return;
    }
    if (dtr) {
        return;
    }

    const bool request_download =
        atomic_load_explicit(&s_download_mode, memory_order_acquire);
    const bsp_usb_reboot_t requested_mode =
        request_download ? BSP_USB_REBOOT_BOOTLOADER : BSP_USB_REBOOT_NORMAL;
    int expected_mode = BSP_USB_REBOOT_NONE;
    if (!atomic_compare_exchange_strong_explicit(&s_reboot_mode, &expected_mode, requested_mode,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    /* Wait for the final idle state, with a fallback if the host drops it. */
    if (esp_timer_start_once(s_restart_timer, BSP_USB_RESTART_FALLBACK_US) != ESP_OK) {
        atomic_store_explicit(&s_reboot_mode, BSP_USB_REBOOT_NONE, memory_order_release);
    }
}

static esp_err_t auto_download_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = restart_timer_callback,
        .name = "usb_reboot",
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_restart_timer);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_register_shutdown_handler(usb_console_before_restart);
    if (ret != ESP_OK) {
        esp_timer_delete(s_restart_timer);
        s_restart_timer = NULL;
    }
    return ret;
}

static void auto_download_deinit(void)
{
    (void)esp_unregister_shutdown_handler(usb_console_before_restart);
    if (s_restart_timer) {
        (void)esp_timer_delete(s_restart_timer);
        s_restart_timer = NULL;
    }
    atomic_store_explicit(&s_reboot_mode, BSP_USB_REBOOT_NONE, memory_order_release);
    atomic_store_explicit(&s_download_mode, false, memory_order_release);
}

#endif /* CONFIG_BSP_USB_AUTO_DOWNLOAD */

static esp_err_t usb_serial_init(void)
{
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_BASE);
    if (ret != ESP_OK) {
        return ret;
    }

    const int written = snprintf(s_usb_serial, sizeof(s_usb_serial),
                                 "%02X%02X%02X%02X%02X%02X",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return written == BSP_USB_SERIAL_LENGTH ? ESP_OK : ESP_FAIL;
}

static void wait_for_cdc_host(void)
{
#if CONFIG_BSP_USB_CONSOLE_HOST_WAIT_MS > 0
    const TickType_t poll_ticks = pdMS_TO_TICKS(10);
    const TickType_t timeout_ticks = pdMS_TO_TICKS(CONFIG_BSP_USB_CONSOLE_HOST_WAIT_MS);
    const TickType_t start_ticks = xTaskGetTickCount();

    while (!tud_cdc_n_connected(TINYUSB_CDC_ACM_0) &&
           (xTaskGetTickCount() - start_ticks) < timeout_ticks) {
        vTaskDelay(poll_ticks > 0 ? poll_ticks : 1);
    }
#endif
}

esp_err_t bsp_usb_console_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = usb_serial_init();
    if (ret != ESP_OK) {
        return ret;
    }

#if CONFIG_BSP_USB_AUTO_DOWNLOAD
    ret = auto_download_init();
    if (ret != ESP_OK) {
        return ret;
    }
    tinyusb_config_t tusb_config = TINYUSB_DEFAULT_CONFIG();
    tusb_config.descriptor.device = &s_usb_device_descriptor;
#else
    tinyusb_config_t tusb_config = TINYUSB_DEFAULT_CONFIG();
#endif
    tusb_config.descriptor.string = s_usb_string_descriptors;
    tusb_config.descriptor.string_count =
        sizeof(s_usb_string_descriptors) / sizeof(s_usb_string_descriptors[0]);

    ret = tinyusb_driver_install(&tusb_config);
    if (ret != ESP_OK) {
#if CONFIG_BSP_USB_AUTO_DOWNLOAD
        auto_download_deinit();
#endif
        return ret;
    }

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
#if CONFIG_BSP_USB_AUTO_DOWNLOAD
        .callback_line_state_changed = cdc_line_state_changed_callback,
#endif
    };
    ret = tinyusb_cdcacm_init(&cdc_config);
    if (ret != ESP_OK) {
        (void)tinyusb_driver_uninstall();
#if CONFIG_BSP_USB_AUTO_DOWNLOAD
        auto_download_deinit();
#endif
        return ret;
    }

    wait_for_cdc_host();

    ret = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if (ret != ESP_OK) {
        (void)tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        (void)tinyusb_driver_uninstall();
#if CONFIG_BSP_USB_AUTO_DOWNLOAD
        auto_download_deinit();
#endif
        return ret;
    }

    /* Send each application log to TinyUSB without stdio buffering. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    s_initialized = true;
    ESP_LOGI(TAG, "USB-OTG CDC console ready%s",
#if CONFIG_BSP_USB_AUTO_DOWNLOAD
             "; USB-Serial/JTAG-compatible reset enabled"
#else
             ""
#endif
    );
    return ESP_OK;
}

bool bsp_usb_console_is_initialized(void)
{
    return s_initialized;
}

#if CONFIG_BSP_USB_CONSOLE_AUTO_INIT

extern void __real_app_main(void);

/*
 * IDF calls app_main from its FreeRTOS main task. Linker wrapping puts TinyUSB
 * initialization at the exact point where the scheduler is available but the
 * application has not started yet.
 */
void __wrap_app_main(void)
{
    ESP_ERROR_CHECK(bsp_usb_console_init());
    __real_app_main();
}

#endif /* CONFIG_BSP_USB_CONSOLE_AUTO_INIT */

#else /* CONFIG_BSP_USB_CONSOLE */

esp_err_t bsp_usb_console_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool bsp_usb_console_is_initialized(void)
{
    return false;
}

#endif /* CONFIG_BSP_USB_CONSOLE */
