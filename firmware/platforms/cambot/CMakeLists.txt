cmake_minimum_required(VERSION 3.16)

# ── Board selection ───────────────────────────────────────────────────────────
#
#  Usage:
#    idf.py set-target esp32s3 build
#
#  Override board:
#    idf.py -DBOARD=esp32s3_cambot set-target esp32s3 build
#
#  Override device number (for mDNS hostname cambot<N>.local):
#    idf.py -DDEVICE_NUMBER=2 set-target esp32s3 build

if(NOT DEFINED BOARD)
    set(BOARD "esp32s3_cambot")
endif()

set(BOARD ${BOARD} CACHE STRING "Active board name" FORCE)

# Resolve board header path (relative to project root)
set(BOARD_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}")
if(NOT EXISTS "${BOARD_CONFIG_DIR}/board_config.h")
    message(FATAL_ERROR "Board '${BOARD}' not found — expected: ${BOARD_CONFIG_DIR}/board_config.h")
endif()

# Pass the include path as a quoted string macro so config.h can #include it
add_compile_definitions(
    BOARD_CONFIG_HEADER="${BOARD_CONFIG_DIR}/board_config.h"
)

message(STATUS "━━━ Board: ${BOARD}  (${BOARD_CONFIG_DIR}/board_config.h) ━━━")

# ── Camera model selection (-DCAMERA=ov3660 or -DCAMERA_MODEL=ov3660) ────────
if(DEFINED CAMERA)
    string(TOLOWER "${CAMERA}" _CAM)
    if(_CAM STREQUAL "ov3660")
        add_compile_definitions(CAMBOT_CAMERA_OV3660=1)
        message(STATUS "━━━ Camera Model: OV3660 ━━━")
    elseif(_CAM STREQUAL "ov2640")
        add_compile_definitions(CAMBOT_CAMERA_OV2640=1)
        message(STATUS "━━━ Camera Model: OV2640 ━━━")
    endif()
elseif(DEFINED CAMERA_MODEL)
    string(TOLOWER "${CAMERA_MODEL}" _CAM)
    if(_CAM STREQUAL "ov3660")
        add_compile_definitions(CAMBOT_CAMERA_OV3660=1)
        message(STATUS "━━━ Camera Model: OV3660 ━━━")
    elseif(_CAM STREQUAL "ov2640")
        add_compile_definitions(CAMBOT_CAMERA_OV2640=1)
        message(STATUS "━━━ Camera Model: OV2640 ━━━")
    endif()
elseif(DEFINED CAM)
    string(TOLOWER "${CAM}" _CAM)
    if(_CAM STREQUAL "ov3660")
        add_compile_definitions(CAMBOT_CAMERA_OV3660=1)
        message(STATUS "━━━ Camera Model: OV3660 ━━━")
    endif()
endif()

if(DEFINED DEVICE_NUMBER)
    add_compile_definitions(DEVICE_NUMBER=${DEVICE_NUMBER})
    message(STATUS "━━━ Device Number: ${DEVICE_NUMBER} ━━━")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cambot)
