idf_component_register(
    SRCS
        "mosaico_camera.c"
        "mosaico_camera_jpeg.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp-mosaico-bsp
        mosaico_module_mgr
        esp_driver_cam
        esp_driver_jpeg
        esp_driver_ppa
        esp_cam_sensor
        esp_video
)
