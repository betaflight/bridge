# Top-level project CMake for betaflight-bridge
#
# This project vendors ESP-IDF as a git submodule under ./esp-idf so builds are
# reproducible without a system-wide IDF install. See README.md for setup.

cmake_minimum_required(VERSION 3.16)

# Target board: a subdirectory of boards/. Selects the flash size, partition
# table, PSRAM and board identity. Override with: idf.py -DBOARD=<name> ...
# (delete ./sdkconfig and reconfigure when switching boards).
set(BOARD "esp32s3-zero" CACHE STRING "Target board (a subdir of boards/)")

file(GLOB _board_dirs RELATIVE "${CMAKE_CURRENT_LIST_DIR}/boards" "${CMAKE_CURRENT_LIST_DIR}/boards/*")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}/sdkconfig.defaults")
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Available boards: ${_board_dirs}")
endif()

# Common defaults first, then the board's overrides (later files win).
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Per-board dependency lock: each board declares its own managed components in
# boards/<board>/idf_component.yml, so a shared lock file would be rewritten on
# every board switch.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${BOARD}")

# The selected board's component name, as a build property: components resolve
# their requirements in a separate CMake process that cannot see ${BOARD}.
idf_build_set_property(BRIDGE_BOARD "${BOARD}")

# Sources live under src/main rather than the default ./main. The selected board
# joins the build as a component of its own, so board-specific sources, headers
# and managed components stay inside boards/<board>/.
set(EXTRA_COMPONENT_DIRS "src" "boards/${BOARD}")

project(betaflight-bridge)

# GCC 14.2 crashes in register allocation when compiling esp_lcd_panel_rgb.c
# with debug optimization. Keep this IDF component unoptimized as a workaround
# when the selected target actually includes esp_lcd.
idf_build_get_property(_components BUILD_COMPONENTS)
if("esp_lcd" IN_LIST _components)
    idf_component_get_property(_esp_lcd_lib esp_lcd COMPONENT_LIB)
    if(_esp_lcd_lib)
        target_compile_options(${_esp_lcd_lib} PRIVATE -O0)
    endif()
endif()

# Boards needing a build tweak that can only run once every component is
# registered (patching a fetched BSP, say) put it in boards/<board>/board.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}/board.cmake" OPTIONAL)
