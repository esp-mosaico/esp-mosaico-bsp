/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/subboard.h"

#include <stddef.h>

#include "bsp/power.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/usb_serial_jtag_ll.h"

static const char *TAG = "bsp_subboard";

typedef struct {
    gpio_num_t left;
    gpio_num_t right;
} bsp_subboard_gpio_pair_t;

static const bsp_subboard_slot_config_t s_slot_configs[BSP_SUBBOARD_SLOT_COUNT] = {
    [BSP_SUBBOARD_SLOT_LEFT] = {
        .slot = BSP_SUBBOARD_SLOT_LEFT,
        .eeprom_addr = BSP_SUBBOARD_EEPROM_ADDR_LEFT,
        .address_select_gpio = BSP_SUBBOARD_ADDR_GPIO_LEFT,
        .address_select_level = BSP_SUBBOARD_ADDR_LEVEL_LEFT,
        .connector_gpio = {
            GPIO_NUM_53, GPIO_NUM_48, GPIO_NUM_13,
            GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_4,
        },
        .rotated_180 = false,
    },
    [BSP_SUBBOARD_SLOT_RIGHT] = {
        .slot = BSP_SUBBOARD_SLOT_RIGHT,
        .eeprom_addr = BSP_SUBBOARD_EEPROM_ADDR_RIGHT,
        .address_select_gpio = BSP_SUBBOARD_ADDR_GPIO_RIGHT,
        .address_select_level = BSP_SUBBOARD_ADDR_LEVEL_RIGHT,
        .connector_gpio = {
            GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_11,
            GPIO_NUM_10, GPIO_NUM_39, GPIO_NUM_5,
        },
        .rotated_180 = true,
    },
};

/*
 * Left-slot canonical GPIOs and their right-slot mirrors.
 * H-connector pairs come from the BSP slot tables; extended pairs come from
 * the dual-slot IO loopback fixture used on the CoreBoard.
 */
static const bsp_subboard_gpio_pair_t s_gpio_pairs[] = {
    {GPIO_NUM_53, GPIO_NUM_46}, /* H2 */
    {GPIO_NUM_48, GPIO_NUM_47}, /* H4 */
    {GPIO_NUM_13, GPIO_NUM_11}, /* H6 */
    {GPIO_NUM_12, GPIO_NUM_10}, /* H8 */
    {GPIO_NUM_14, GPIO_NUM_39}, /* H10 / EEPROM A0 */
    {GPIO_NUM_4,  GPIO_NUM_5},  /* H12 / WS2812 */
    {GPIO_NUM_16, GPIO_NUM_40}, /* KEY1 / camera D0 */
    {GPIO_NUM_15, GPIO_NUM_38}, /* KEY2 / camera D1 */
    {GPIO_NUM_17, GPIO_NUM_37}, /* camera PCLK pair */
    {GPIO_NUM_18, GPIO_NUM_54}, /* camera D6 pair */
    {GPIO_NUM_19, GPIO_NUM_52}, /* camera DE pair */
    {GPIO_NUM_55, GPIO_NUM_49}, /* camera VSYNC pair */
};

static portMUX_TYPE s_resource_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_subboard_initialized;
static bool s_camera_claimed;
static bool s_camera_flash_initialized;
static bool s_usj_pad_was_enabled;
static bool s_usj_clock_was_enabled;
static uint32_t s_usj_intr_enable_mask;

static esp_err_t bsp_subboard_camera_flash_force_off(void)
{
    const gpio_config_t flash_config = {
        .pin_bit_mask = BIT64(GPIO_NUM_34),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_34, 1), TAG,
                        "preset camera flash off failed");
    ESP_RETURN_ON_ERROR(gpio_config(&flash_config), TAG,
                        "configure camera flash GPIO failed");
    s_camera_flash_initialized = true;
    return ESP_OK;
}

static esp_err_t configure_address_select(const bsp_subboard_slot_config_t *slot)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(slot->address_select_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(slot->address_select_gpio, slot->address_select_level),
                        TAG, "preset slot %d address GPIO failed", slot->slot);
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG,
                        "configure slot %d address GPIO failed", slot->slot);
    ESP_LOGD(TAG, "Address select: slot=%d GPIO%d=%u -> EEPROM 0x%02X",
             slot->slot, slot->address_select_gpio, slot->address_select_level,
             slot->eeprom_addr);
    return ESP_OK;
}

esp_err_t bsp_subboard_init(void)
{
    portENTER_CRITICAL(&s_resource_lock);
    const bool initialized = s_subboard_initialized;
    portEXIT_CRITICAL(&s_resource_lock);
    if (initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG,
                        "enable subboard VCC rail failed");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "initialize shared I2C failed");

    for (size_t i = 0; i < BSP_SUBBOARD_SLOT_COUNT; ++i) {
        bool skip = false;
        portENTER_CRITICAL(&s_resource_lock);
        skip = s_camera_claimed && i == BSP_SUBBOARD_SLOT_LEFT;
        portEXIT_CRITICAL(&s_resource_lock);
        if (!skip) {
            ESP_RETURN_ON_ERROR(configure_address_select(&s_slot_configs[i]), TAG,
                                "initialize slot %u failed", (unsigned)i);
        }
    }

    portENTER_CRITICAL(&s_resource_lock);
    s_subboard_initialized = true;
    portEXIT_CRITICAL(&s_resource_lock);
    ESP_LOGI(TAG,
             "Subboard address select ready: left GPIO%d=%d -> 0x%02X, "
             "right GPIO%d=%d -> 0x%02X",
             BSP_SUBBOARD_ADDR_GPIO_LEFT, BSP_SUBBOARD_ADDR_LEVEL_LEFT,
             BSP_SUBBOARD_EEPROM_ADDR_LEFT,
             BSP_SUBBOARD_ADDR_GPIO_RIGHT, BSP_SUBBOARD_ADDR_LEVEL_RIGHT,
             BSP_SUBBOARD_EEPROM_ADDR_RIGHT);
    return ESP_OK;
}

esp_err_t bsp_subboard_get_slot_config(bsp_subboard_slot_t slot,
                                       bsp_subboard_slot_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config && slot >= BSP_SUBBOARD_SLOT_LEFT &&
                            slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid slot config request");
    *out_config = s_slot_configs[slot];
    return ESP_OK;
}

esp_err_t bsp_subboard_apply_address_select(bsp_subboard_slot_t slot)
{
    ESP_RETURN_ON_FALSE(slot >= BSP_SUBBOARD_SLOT_LEFT &&
                            slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid address-select slot");

    portENTER_CRITICAL(&s_resource_lock);
    const bool camera_blocks_left =
        s_camera_claimed && slot == BSP_SUBBOARD_SLOT_LEFT;
    portEXIT_CRITICAL(&s_resource_lock);
    if (camera_blocks_left) {
        ESP_LOGE(TAG, "Cannot restore left address select while camera claimed: %s",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(configure_address_select(&s_slot_configs[slot]), TAG,
                        "apply address select for slot %d failed", slot);
    ESP_LOGI(TAG, "Address select applied: slot=%d GPIO%d=%u EEPROM=0x%02X",
             slot, s_slot_configs[slot].address_select_gpio,
             s_slot_configs[slot].address_select_level,
             s_slot_configs[slot].eeprom_addr);
    return ESP_OK;
}

esp_err_t bsp_subboard_slot_from_eeprom_addr(uint8_t eeprom_addr,
                                             bsp_subboard_slot_t *out_slot)
{
    ESP_RETURN_ON_FALSE(out_slot, ESP_ERR_INVALID_ARG, TAG,
                        "slot output is null");
    if (eeprom_addr == BSP_SUBBOARD_EEPROM_ADDR_LEFT) {
        *out_slot = BSP_SUBBOARD_SLOT_LEFT;
        return ESP_OK;
    }
    if (eeprom_addr == BSP_SUBBOARD_EEPROM_ADDR_RIGHT) {
        *out_slot = BSP_SUBBOARD_SLOT_RIGHT;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Unknown subboard EEPROM address 0x%02X: %s", eeprom_addr,
             esp_err_to_name(ESP_ERR_NOT_FOUND));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t bsp_subboard_eeprom_addr_from_slot(bsp_subboard_slot_t slot,
                                             uint8_t *out_addr)
{
    ESP_RETURN_ON_FALSE(out_addr && slot >= BSP_SUBBOARD_SLOT_LEFT &&
                            slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid EEPROM address request");
    *out_addr = s_slot_configs[slot].eeprom_addr;
    return ESP_OK;
}

gpio_num_t bsp_subboard_map_gpio(bsp_subboard_slot_t slot, gpio_num_t left_gpio)
{
    if (slot != BSP_SUBBOARD_SLOT_RIGHT) {
        return left_gpio;
    }

    for (size_t i = 0; i < sizeof(s_gpio_pairs) / sizeof(s_gpio_pairs[0]); ++i) {
        if (s_gpio_pairs[i].left == left_gpio) {
            return s_gpio_pairs[i].right;
        }
    }
    return left_gpio;
}

esp_err_t bsp_subboard_camera_acquire(bsp_subboard_slot_t slot,
                                      bsp_subboard_camera_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config, ESP_ERR_INVALID_ARG, TAG,
                        "camera config output is null");
    ESP_RETURN_ON_FALSE(slot == BSP_SUBBOARD_SLOT_LEFT, ESP_ERR_NOT_SUPPORTED, TAG,
                        "camera subboard supports the left slot only");
    ESP_RETURN_ON_ERROR(bsp_subboard_init(), TAG, "initialize subboard resources failed");

    if (!s_camera_flash_initialized) {
        ESP_RETURN_ON_ERROR(bsp_subboard_camera_flash_force_off(), TAG,
                            "initialize camera flash GPIO failed");
    }

    portENTER_CRITICAL(&s_resource_lock);
    if (s_camera_claimed) {
        portEXIT_CRITICAL(&s_resource_lock);
        ESP_LOGE(TAG, "Camera DVP resource is already claimed: %s",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }
    s_camera_claimed = true;
    s_usj_pad_was_enabled = usb_serial_jtag_ll_phy_is_pad_enabled();
    s_usj_clock_was_enabled = usb_serial_jtag_ll_module_is_enabled();
    s_usj_intr_enable_mask =
        s_usj_clock_was_enabled
            ? usb_serial_jtag_ll_get_intr_ena_status()
            : 0;
    portEXIT_CRITICAL(&s_resource_lock);

    /*
     * Camera D2 shares GPIO33 with the built-in USB Serial/JTAG PHY. The BSP
     * application console uses USB-OTG, so releasing these pads does not stop
     * the BSP TinyUSB console.
     */
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_phy_enable_pad(false);
    usb_serial_jtag_ll_enable_bus_clock(false);

    *out_config = (bsp_subboard_camera_config_t) {
        .data_io = {
            GPIO_NUM_16, GPIO_NUM_15, GPIO_NUM_33, GPIO_NUM_4,
            GPIO_NUM_14, GPIO_NUM_12, GPIO_NUM_18, GPIO_NUM_13,
        },
        .vsync_io = GPIO_NUM_55,
        .de_io = GPIO_NUM_19,
        .pclk_io = GPIO_NUM_17,
        .xclk_io = GPIO_NUM_NC,
        .reset_io = GPIO_NUM_53,
        .pwdn_io = GPIO_NUM_48,
        /* Flash LED is wired to GPIO34 only (active-low). */
        .flash_io = GPIO_NUM_34,
        .xclk_freq_hz = 0,
        /*
         * XCLK is supplied by the CameraBoard's 24 MHz oscillator. Keep the
         * stabilization interval explicit so SCCB is never accessed
         * immediately after the board resource is acquired.
         */
        .xclk_stabilization_ms = 20,
        .sccb_freq_hz = 100000,
    };

    ESP_LOGI(TAG, "Camera DVP resource claimed on left slot (EEPROM A0 GPIO14 reused as D4)");
    return ESP_OK;
}

esp_err_t bsp_subboard_button_led_get_config(bsp_subboard_slot_t slot,
                                             bsp_subboard_button_led_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config, ESP_ERR_INVALID_ARG, TAG,
                        "button LED config output is null");
    ESP_RETURN_ON_FALSE(slot >= BSP_SUBBOARD_SLOT_LEFT &&
                            slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid button LED slot");

    bsp_subboard_slot_config_t slot_config = {0};
    ESP_RETURN_ON_ERROR(bsp_subboard_get_slot_config(slot, &slot_config), TAG,
                        "get slot config failed");

    *out_config = (bsp_subboard_button_led_config_t) {
        .key1_io = bsp_subboard_map_gpio(slot, GPIO_NUM_12),
        .key2_io = bsp_subboard_map_gpio(slot, GPIO_NUM_15),
        .ws2812_io = bsp_subboard_map_gpio(slot, GPIO_NUM_4),
        .led_count = 3,
        .eeprom_addr = slot_config.eeprom_addr,
        .rotated_180 = slot_config.rotated_180,
    };

    ESP_LOGI(TAG,
             "Button LED pins: slot=%d eeprom=0x%02X KEY1=%d KEY2=%d WS2812=%d leds=%u",
             slot, out_config->eeprom_addr, out_config->key1_io,
             out_config->key2_io, out_config->ws2812_io, out_config->led_count);
    return ESP_OK;
}

esp_err_t bsp_subboard_joystick_get_config(bsp_subboard_slot_t slot,
                                           bsp_subboard_joystick_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config, ESP_ERR_INVALID_ARG, TAG,
                        "joystick config output is null");
    ESP_RETURN_ON_FALSE(slot >= BSP_SUBBOARD_SLOT_LEFT &&
                            slot < BSP_SUBBOARD_SLOT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid joystick slot");

    bsp_subboard_slot_config_t slot_config = {0};
    ESP_RETURN_ON_ERROR(bsp_subboard_get_slot_config(slot, &slot_config), TAG,
                        "get slot config failed");

    *out_config = (bsp_subboard_joystick_config_t) {
        .x_io = bsp_subboard_map_gpio(slot, GPIO_NUM_48),
        .y_io = bsp_subboard_map_gpio(slot, GPIO_NUM_53),
        /*
         * The right connector is rotated, but its ADC N-side polarity changes
         * cancel the physical axis flips. Keep the normalized directions
         * identical to the validated dual-joystick example.
         */
        .invert_x = false,
        .invert_y = false,
        .buttons = {
            {bsp_subboard_map_gpio(slot, GPIO_NUM_16), true},
            {bsp_subboard_map_gpio(slot, GPIO_NUM_4), false},
            {bsp_subboard_map_gpio(slot, GPIO_NUM_15), false},
            {bsp_subboard_map_gpio(slot, GPIO_NUM_12), false},
            {bsp_subboard_map_gpio(slot, GPIO_NUM_13), false},
        },
        .eeprom_addr = slot_config.eeprom_addr,
        .rotated_180 = slot_config.rotated_180,
    };

    ESP_LOGI(TAG,
             "Joystick pins: slot=%d eeprom=0x%02X X=%d Y=%d buttons=%d/%d/%d/%d/%d",
             slot, out_config->eeprom_addr, out_config->x_io, out_config->y_io,
             out_config->buttons[0].io, out_config->buttons[1].io,
             out_config->buttons[2].io, out_config->buttons[3].io,
             out_config->buttons[4].io);
    return ESP_OK;
}

esp_err_t bsp_subboard_camera_release(bsp_subboard_slot_t slot)
{
    ESP_RETURN_ON_FALSE(slot == BSP_SUBBOARD_SLOT_LEFT, ESP_ERR_NOT_SUPPORTED, TAG,
                        "camera subboard supports the left slot only");

    portENTER_CRITICAL(&s_resource_lock);
    if (!s_camera_claimed) {
        portEXIT_CRITICAL(&s_resource_lock);
        return ESP_OK;
    }
    const bool restore_pad = s_usj_pad_was_enabled;
    const bool restore_clock = s_usj_clock_was_enabled;
    const uint32_t restore_intr_mask = s_usj_intr_enable_mask;
    s_camera_claimed = false;
    portEXIT_CRITICAL(&s_resource_lock);

    ESP_RETURN_ON_ERROR(bsp_subboard_apply_address_select(BSP_SUBBOARD_SLOT_LEFT),
                        TAG, "restore left EEPROM address GPIO failed");
    if (restore_clock) {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
    usb_serial_jtag_ll_phy_enable_pad(restore_pad);
    if (restore_clock && restore_intr_mask) {
        usb_serial_jtag_ll_ena_intr_mask(restore_intr_mask);
    }
    ESP_LOGI(TAG, "Camera DVP resource released; left EEPROM discovery restored");
    return ESP_OK;
}
