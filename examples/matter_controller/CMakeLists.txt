cmake_minimum_required(VERSION 3.16)

# Matter Controller uses ESP RainMaker for provisioning QR prompts.
set(APP_SCAN_QRCODE_RAINMAKER 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults)
include(${CMAKE_CURRENT_LIST_DIR}/../common/boards/add_board_sdkconfig_defaults.cmake)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common"
                         "${CMAKE_CURRENT_LIST_DIR}/../../components")

project(matter_controller)

idf_build_set_property(COMPILE_DEFINITIONS "APP_SCAN_QRCODE_RAINMAKER=1" APPEND)
