set(BSP_SRCS
    "esp32_s31_mosaico.c"
    "subboard.c"
    "usb_console.c"
    "display.c"
    "audio.c"
    "nand_flash.c"
    "sensors.c"
    "battery.c"
)

set(BSP_REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_spi
    esp_hw_support
    esp_hal_usb
    esp_lcd
    esp_system
    esp_timer
    bq27220
    espressif__button
    espressif__esp_codec_dev
    esp_lcd_co5300
    espressif__esp_lvgl_adapter
    espressif__spi_nand_flash
    espressif2022__bmi270
    tangyumei3535__bmm150_sensorapi
    waveshare__esp_lcd_touch_cst9217
)

idf_component_register(
    SRCS ${BSP_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${BSP_REQUIRES}
)

if(CONFIG_BSP_USB_CONSOLE)
    # The component manager selects esp_tinyusb through the matching Kconfig
    # rule. Add the build dependency after component discovery so its headers
    # are available without making TinyUSB mandatory for every BSP user.
    idf_component_optional_requires(PUBLIC espressif__esp_tinyusb)
endif()

if(CONFIG_BSP_USB_CONSOLE_AUTO_INIT)
    # Run BSP initialization from IDF's main task immediately before the user
    # app_main. -u also retains usb_console.c when no application API is used.
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=app_main"
        "-u __wrap_app_main"
    )
endif()
