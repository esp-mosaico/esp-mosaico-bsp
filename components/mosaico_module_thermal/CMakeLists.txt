idf_component_register(SRCS "mosaico_thermal.c" "MLX90640_API.c" "MLX90640_I2C_Driver.c"
                       INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "private"
                       PRIV_REQUIRES esp_driver_i2c esp_timer freertos)
