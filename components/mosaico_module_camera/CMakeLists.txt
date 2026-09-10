idf_component_register(
    SRCS
        "mosaico_camera.c"
        "mosaico_camera_jpeg.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_cam
        esp_driver_jpeg
        esp_driver_ppa
    PRIV_REQUIRES
        esp_hw_support
)
