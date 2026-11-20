idf_component_register(
    SRCS "test_app_main.c" "nand_test_common.c" "test_nand_raw.c" "test_nand_storage.c" "test_nand_manual.c"
    PRIV_INCLUDE_DIRS "." "../../../espressif__spi_nand_flash/priv_include"
    PRIV_REQUIRES unity esp_timer esp_driver_spi esp_driver_gpio nvs_flash mosaico_nand_storage espressif__spi_nand_flash
    WHOLE_ARCHIVE
)
