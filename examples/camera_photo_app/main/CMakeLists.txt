set(srcs
    "main.c"
    "camera_settings.c"
    "camera_ui.c"
    "photo_store.c"
    "ui_icons.c"
)

set(priv_requires
    esp-mosaico-bsp
    esp_driver_ppa
    esp_lcd
    esp_mm
    mosaico_module_camera
    mosaico_module_button_led
    esp_new_jpeg
    nvs_flash
)

if(CONFIG_CAMERA_PHOTO_USB_MSC)
    list(APPEND srcs "photo_usb_msc.c")
    list(APPEND priv_requires esp_tinyusb wear_levelling fatfs esp_partition)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${priv_requires}
)
