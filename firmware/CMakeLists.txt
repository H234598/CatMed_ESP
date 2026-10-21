cmake_minimum_required(VERSION 3.20)
set(COMPONENTS main)
option(KATMED_FULL_STACK "Compile and link the complete dependency probe" ON)
option(KATMED_WITH_CORE "Link the host-tested reducer" ON)
option(KATMED_ACME_RESOURCE_PROBE "Enable the isolated ACME resource source probe" OFF)
option(KATMED_ACME_RESOURCE_CLIENT "Run the real client in the isolated resource probe" OFF)
set(KATMED_ACME_RESOURCE_SERVER "http://127.0.0.1:17171/directory" CACHE STRING
    "Isolated fixture directory for the resource probe")
if(KATMED_ACME_RESOURCE_CLIENT AND NOT KATMED_ACME_RESOURCE_PROBE)
    message(FATAL_ERROR "ACME resource client requires the isolated probe observer")
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include_directories("${CMAKE_BINARY_DIR}/generated")
idf_build_set_property(KATMED_FULL_STACK "${KATMED_FULL_STACK}")
idf_build_set_property(KATMED_WITH_CORE "${KATMED_WITH_CORE}")
idf_build_set_property(KATMED_ACME_RESOURCE_PROBE "${KATMED_ACME_RESOURCE_PROBE}")
idf_build_set_property(KATMED_ACME_RESOURCE_CLIENT "${KATMED_ACME_RESOURCE_CLIENT}")
idf_build_set_property(KATMED_ACME_RESOURCE_SERVER "${KATMED_ACME_RESOURCE_SERVER}")
set(DEPENDENCIES_LOCK "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.${IDF_TARGET}.lock")
idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK}")
project(katmed_foundation)
set(KATMED_USERFS_SEED_DIR "${CMAKE_BINARY_DIR}/katmed_userfs_seed")
file(MAKE_DIRECTORY "${KATMED_USERFS_SEED_DIR}")
littlefs_create_partition_image(user_images "${KATMED_USERFS_SEED_DIR}")
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/prepare_acme_component.py"
            --component "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/dannybackx__acmeclient"
    RESULT_VARIABLE KATMED_ACME_PATCH_RESULT)
if(NOT KATMED_ACME_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Verified ACME 1.1.1 atomic persistence bridge failed")
endif()
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/prepare_iot_bridge_compat.py"
            --component "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/espressif__iot_bridge"
    RESULT_VARIABLE KATMED_IOT_BRIDGE_PATCH_RESULT)
if(NOT KATMED_IOT_BRIDGE_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Verified IoT Bridge 1.0.1 IDF 6.1 compatibility patch failed")
endif()
execute_process(
    COMMAND /usr/bin/python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/prepare_lvgl_compat.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/lvgl__lvgl"
    RESULT_VARIABLE KATMED_LVGL_COMPAT_RESULT)
if(NOT KATMED_LVGL_COMPAT_RESULT EQUAL 0)
    message(FATAL_ERROR "Pinned LVGL TJpgD compatibility check failed")
endif()
