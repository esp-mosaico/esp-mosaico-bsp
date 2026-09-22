# Example-owned, pinned dependencies. Never search adjacent repositories.
include_guard(GLOBAL)
include(FetchContent)
file(READ "${CMAKE_CURRENT_LIST_DIR}/game_dependencies.json" _mosaico_game_lock)

function(_mosaico_game_dependency name variable required_file)
    if(${variable})
        get_filename_component(source "${${variable}}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        string(JSON repository GET "${_mosaico_game_lock}" "${name}" repository)
        string(JSON revision GET "${_mosaico_game_lock}" "${name}" revision)
        string(LENGTH "${revision}" revision_length)
        if(NOT revision_length EQUAL 40 OR NOT revision MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "Game dependency ${name} must pin a full Git revision")
        endif()
        FetchContent_Declare(mosaico_game_${name}
            GIT_REPOSITORY "${repository}"
            GIT_TAG "${revision}"
            GIT_SUBMODULES ""
            SOURCE_SUBDIR _mosaico_download_only)
        FetchContent_MakeAvailable(mosaico_game_${name})
        set(source "${mosaico_game_${name}_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${source}/${required_file}")
        message(FATAL_ERROR "${variable} does not contain ${required_file}: ${source}")
    endif()
    set(${variable} "${source}" PARENT_SCOPE)
endfunction()

# A workspace can explicitly supply its own fixed checkouts. A standalone BSP
# clone downloads the example's declared revisions into the build directory.
_mosaico_game_dependency(engine RAYLIB_LITE_ENGINE_ROOT cmake/mosaico_game_sdk.cmake)
if(NOT MOSAICO_GAME_HOST_ONLY)
    _mosaico_game_dependency(utils MOSAICO_UTILS_ROOT mosaico-tools/cmake/raylib_lite_engine.cmake)
    list(APPEND EXTRA_COMPONENT_DIRS "${MOSAICO_UTILS_ROOT}/ESP-Iris/components/esp_iris")
endif()
