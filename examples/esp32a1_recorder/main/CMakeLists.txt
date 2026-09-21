idf_component_register(
    SRCS "app_main.c" "a1_rec_audio.c" "recorder.c" "recorder_ui.c" "recorder_key.c" "nand_littlefs.c" "nand_lfs_probe.c" "wav_file.c" "pcm_cache.c" "recording_storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp-mosaico-bsp esp32_a1 esp_codec_dev littlefs si12t esp_timer
)
# Use the same LittleFS implementation as the VFS for CRC-checked, read-only
# geometry discovery. Do not modify managed component sources or on-disk data.
idf_component_get_property(littlefs_dir joltwallet__littlefs COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${littlefs_dir}/src/littlefs")
