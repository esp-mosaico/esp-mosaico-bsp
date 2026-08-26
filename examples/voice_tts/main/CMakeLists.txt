idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)

# Keep ESP-DL compatible with GCC 16's stricter C++ linkage diagnostic.
idf_component_get_property(esp_dl_lib espressif__esp-dl COMPONENT_LIB)
target_compile_options(${esp_dl_lib} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=non-c-typedef-for-linkage>)

idf_build_get_property(build_components BUILD_COMPONENTS)
if(esp-sr IN_LIST build_components)
    set(esp_sr_component esp-sr)
else()
    set(esp_sr_component espressif__esp-sr)
endif()

idf_component_get_property(esp_sr_path ${esp_sr_component} COMPONENT_DIR)
set(voice_data_image
    "${esp_sr_path}/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat")

add_custom_target(voice_data ALL DEPENDS "${voice_data_image}")
add_dependencies(flash voice_data)

partition_table_get_partition_info(voice_data_size
    "--partition-name voice_data" "size")
partition_table_get_partition_info(voice_data_offset
    "--partition-name voice_data" "offset")

if("${voice_data_size}" AND "${voice_data_offset}")
    esptool_py_flash_to_partition(flash "voice_data" "${voice_data_image}")
else()
    message(FATAL_ERROR
        "Partition table must contain a voice_data data partition")
endif()
