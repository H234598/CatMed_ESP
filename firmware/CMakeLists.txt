cmake_minimum_required(VERSION 3.20)
set(COMPONENTS main)
option(KATMED_FULL_STACK "Compile and link the complete dependency probe" ON)
option(KATMED_WITH_CORE "Link the host-tested reducer" ON)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include_directories("${CMAKE_BINARY_DIR}/generated")
idf_build_set_property(KATMED_FULL_STACK "${KATMED_FULL_STACK}")
idf_build_set_property(KATMED_WITH_CORE "${KATMED_WITH_CORE}")
set(DEPENDENCIES_LOCK "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.${IDF_TARGET}.lock")
idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK}")
project(katmed_foundation)
execute_process(
    COMMAND /usr/bin/python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/prepare_lvgl_compat.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/lvgl__lvgl"
    RESULT_VARIABLE KATMED_LVGL_COMPAT_RESULT)
if(NOT KATMED_LVGL_COMPAT_RESULT EQUAL 0)
    message(FATAL_ERROR "Pinned LVGL TJpgD compatibility check failed")
endif()
