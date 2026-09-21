idf_component_register(
    SRCS "app_main.c" "a1_audio.c" "music_player.c" "music_ui.c" "loading_font.c" "nand_littlefs.c" "nand_lfs_probe.c" "touch_keys.c"
    INCLUDE_DIRS "."
    REQUIRES esp-mosaico-bsp esp32_a1 esp_codec_dev littlefs si12t
)
idf_component_get_property(littlefs_dir joltwallet__littlefs COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${littlefs_dir}/src/littlefs")
