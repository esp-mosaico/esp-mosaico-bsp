set(BSP_SRCS
    "onboard/esp_mosaico.c"
    "onboard/subboard.c"
    "onboard/display.c"
    "onboard/audio.c"
    "onboard/nand_flash.c"
    "onboard/sensors.c"
    "onboard/battery.c"
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
