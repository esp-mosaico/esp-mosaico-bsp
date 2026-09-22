idf_component_register(
    SRCS "main.c" "shooter_app.c" "shooter_game.c" "shooter_view.c" "game_audio.c" "raylib_screen_mirror.c"
    INCLUDE_DIRS "."
    REQUIRES mosaico_game_app mosaico_game_assets mosaico_game_2d
             mosaico_raylib_fast mosaico_raylib_port raylib esp-gsp)

set(GAME_ASSET_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../assets_src")
set(GAME_ASSET_OUTPUT "${CMAKE_CURRENT_LIST_DIR}/../assets/generated")
add_custom_target(shooter_assets ALL
    COMMAND "${PYTHON}" "${GAME_ASSET_SOURCE}/prepare_sprites.py"
    COMMAND "${PYTHON}" "${RAYLIB_LITE_ENGINE_ROOT}/tools/pack_game_assets.py"
        --source "${GAME_ASSET_SOURCE}" --output "${GAME_ASSET_OUTPUT}"
    BYPRODUCTS "${GAME_ASSET_OUTPUT}/shooter.atlas"
    DEPENDS "${GAME_ASSET_SOURCE}/game_assets.json"
        "${GAME_ASSET_SOURCE}/atlas.json" "${GAME_ASSET_SOURCE}/prepare_sprites.py")
add_dependencies(${COMPONENT_LIB} shooter_assets)
target_add_binary_data(${COMPONENT_LIB} "${GAME_ASSET_OUTPUT}/shooter.atlas" BINARY
    RENAME_TO shooter_atlas DEPENDS shooter_assets)
target_include_directories(${COMPONENT_LIB} PRIVATE "${GAME_ASSET_OUTPUT}")

set(canvas_png "${CMAKE_CURRENT_BINARY_DIR}/canvas_480.png")
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/make_canvas_png.py" "${canvas_png}"
    RESULT_VARIABLE canvas_result)
if(NOT canvas_result EQUAL 0)
    message(FATAL_ERROR "Could not generate Raylib Canvas placeholder")
endif()
set(CANVAS_IMAGE "${canvas_png}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/scene.json.in"
               "${CMAKE_CURRENT_BINARY_DIR}/scene.json" @ONLY)
gsp_add_bundle(${COMPONENT_LIB}
    SCENES "${CMAKE_CURRENT_BINARY_DIR}/scene.json"
    PIXEL_FORMAT rgb565)
