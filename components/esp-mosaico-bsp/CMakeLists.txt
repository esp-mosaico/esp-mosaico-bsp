set(BSP_SRCS
    "onboard/esp_mosaico.c"
    "onboard/subboard.c"
    "onboard/audio.c"
    "onboard/nand_flash.c"
    "onboard/sensors.c"
    "onboard/battery.c"
)

if(CONFIG_BSP_DISPLAY_ENABLE)
    list(APPEND BSP_SRCS "onboard/display.c")
endif()

set(BSP_REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_spi
    esp_hw_support
    efuse
    esp_lcd
    esp_system
    esp_timer
    mosaico_boot_splash
    soc
)

idf_component_register(
    SRCS ${BSP_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${BSP_REQUIRES}
)
