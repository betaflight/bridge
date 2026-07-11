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

# Per-board dependency lock: boards with a display pull in extra managed
# components (rule-gated in src/main/idf_component.yml), so a shared lock file
# would be rewritten on every board switch.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${BOARD}")

# Sources live under src/main rather than the default ./main.
set(EXTRA_COMPONENT_DIRS "src")

project(betaflight-bridge)

# The Waveshare BSPs use memcpy without including <string.h>; force the
# include rather than patching the fetched component.
idf_build_get_property(_components BUILD_COMPONENTS)
if("waveshare__esp32_s3_touch_lcd_4b" IN_LIST _components)
    idf_component_get_property(_bsp_lib waveshare__esp32_s3_touch_lcd_4b COMPONENT_LIB)
    target_compile_options(${_bsp_lib} PRIVATE "SHELL:-include string.h")
endif()
