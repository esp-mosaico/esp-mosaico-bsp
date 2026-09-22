idf_component_register(
    SRCS "main.c" "tower_app.c" "tower_game.c" "tower_view.c" "tower_audio.c" "tower_screen_mirror.c"
    INCLUDE_DIRS "."
    REQUIRES mosaico_game_app mosaico_game_assets mosaico_game_2d
             mosaico_game_tilemap mosaico_game_audio mosaico_raylib_fast mosaico_raylib_port
             raylib esp-gsp)

set(GAME_ASSET_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../assets_src")
set(GAME_ASSET_OUTPUT "${CMAKE_CURRENT_LIST_DIR}/../assets/generated")
add_custom_target(tower_game_assets ALL
    COMMAND "${PYTHON}" "${RAYLIB_LITE_ENGINE_ROOT}/tools/pack_game_assets.py"
            --source "${GAME_ASSET_SOURCE}" --output "${GAME_ASSET_OUTPUT}"
    BYPRODUCTS "${GAME_ASSET_OUTPUT}/tower.atlas"
               "${GAME_ASSET_OUTPUT}/terrain.atlas"
               "${GAME_ASSET_OUTPUT}/level01.map"
               "${GAME_ASSET_OUTPUT}/build.sound"
               "${GAME_ASSET_OUTPUT}/explosion.sound"
               "${GAME_ASSET_OUTPUT}/game_over.sound"
               "${GAME_ASSET_OUTPUT}/hit.sound"
               "${GAME_ASSET_OUTPUT}/leak.sound"
               "${GAME_ASSET_OUTPUT}/music.sound"
               "${GAME_ASSET_OUTPUT}/shot.sound"
               "${GAME_ASSET_OUTPUT}/wave.sound"
    DEPENDS "${GAME_ASSET_SOURCE}/game_assets.json"
            "${GAME_ASSET_SOURCE}/atlas.json"
            "${GAME_ASSET_SOURCE}/level01.tmj"
            "${GAME_ASSET_SOURCE}/tower_atlas_source.png")
add_dependencies(${COMPONENT_LIB} tower_game_assets)
target_add_binary_data(${COMPONENT_LIB} "${GAME_ASSET_OUTPUT}/tower.atlas" BINARY
    RENAME_TO tower_atlas DEPENDS tower_game_assets)
target_add_binary_data(${COMPONENT_LIB} "${GAME_ASSET_OUTPUT}/terrain.atlas" BINARY
    RENAME_TO tower_terrain DEPENDS tower_game_assets)
target_add_binary_data(${COMPONENT_LIB} "${GAME_ASSET_OUTPUT}/level01.map" BINARY
    RENAME_TO tower_level DEPENDS tower_game_assets)
foreach(sound build explosion game_over hit leak music shot wave)
    target_add_binary_data(${COMPONENT_LIB} "${GAME_ASSET_OUTPUT}/${sound}.sound" BINARY
        RENAME_TO "tower_${sound}" DEPENDS tower_game_assets)
endforeach()
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${GAME_ASSET_OUTPUT}")
spiffs_create_partition_assets(
    game_assets "${GAME_ASSET_OUTPUT}"
    FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".atlas,.map,.sound"
    MMAP_FILE_ALIGNMENT 64
    IMPORT_INC_PATH "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS tower_game_assets)

set(canvas_png "${CMAKE_CURRENT_BINARY_DIR}/canvas_480.png")
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/make_canvas_png.py" "${canvas_png}"
    RESULT_VARIABLE canvas_result)
if(NOT canvas_result EQUAL 0)
    message(FATAL_ERROR "Could not generate Tower Defense Canvas placeholder")
endif()
set(CANVAS_IMAGE "${canvas_png}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/scene.json.in"
               "${CMAKE_CURRENT_BINARY_DIR}/scene.json" @ONLY)
gsp_add_bundle(${COMPONENT_LIB}
    SCENES "${CMAKE_CURRENT_BINARY_DIR}/scene.json"
    PIXEL_FORMAT rgb565)

# Ship the generated resources with the application's partition layout.
set_property(GLOBAL APPEND PROPERTY MOSAICO_SYSTEM_UPDATE_DATA_LABELS game_assets)
set_property(GLOBAL PROPERTY MOSAICO_SYSTEM_UPDATE_DATA_game_assets_IMAGE
    "${CMAKE_BINARY_DIR}/mmap_build/generated/game_assets/game_assets.bin")
set_property(GLOBAL PROPERTY MOSAICO_SYSTEM_UPDATE_DATA_game_assets_TARGET
    assets_game_assets_bin)
