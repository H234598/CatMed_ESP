cmake_minimum_required(VERSION 3.20)
set(COMPONENTS main)
option(KATMED_FULL_STACK "Compile and link the complete dependency probe" ON)
option(KATMED_WITH_CORE "Link the host-tested reducer" ON)
option(KATMED_APPSTORE_MAINTENANCE "Private C6 AppStore maintenance image" OFF)
if(KATMED_APPSTORE_MAINTENANCE)
    if(NOT KATMED_FULL_STACK OR NOT KATMED_WITH_CORE)
        message(FATAL_ERROR "AppStore maintenance requires the full C6 UI and core")
    endif()
    if(NOT IDF_TARGET STREQUAL "esp32c6")
        message(FATAL_ERROR "AppStore maintenance is C6-only")
    endif()
    if(NOT CMAKE_BINARY_DIR MATCHES "appmaint")
        message(FATAL_ERROR "AppStore maintenance requires a separate appmaint build directory")
    endif()
    if(NOT IS_ABSOLUTE "${KATMED_APPSTORE_TARGET_FILE}" OR
       NOT EXISTS "${KATMED_APPSTORE_TARGET_FILE}")
        message(FATAL_ERROR "Protected external MAC48 build input is required")
    endif()
    file(REAL_PATH "${KATMED_APPSTORE_TARGET_FILE}" KATMED_APPSTORE_TARGET_REAL)
    file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" KATMED_APPSTORE_SOURCE_REAL)
    string(FIND "${KATMED_APPSTORE_TARGET_REAL}" "${KATMED_APPSTORE_SOURCE_REAL}/" KATMED_APPSTORE_IN_SOURCE)
    if(KATMED_APPSTORE_IN_SOURCE EQUAL 0)
        message(FATAL_ERROR "MAC48 build input must be outside source checkout")
    endif()
    get_filename_component(KATMED_APPSTORE_TARGET_DIR "${KATMED_APPSTORE_TARGET_REAL}" DIRECTORY)
    execute_process(COMMAND git -C "${KATMED_APPSTORE_TARGET_DIR}" rev-parse --is-inside-work-tree
        OUTPUT_VARIABLE KATMED_APPSTORE_INPUT_GIT OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(KATMED_APPSTORE_INPUT_GIT STREQUAL "true")
        execute_process(COMMAND git -C "${KATMED_APPSTORE_TARGET_DIR}" check-ignore --quiet
            "${KATMED_APPSTORE_TARGET_REAL}" RESULT_VARIABLE KATMED_APPSTORE_IGNORED)
        if(NOT KATMED_APPSTORE_IGNORED EQUAL 0)
            message(FATAL_ERROR "MAC48 build input inside Git must be ignored")
        endif()
    endif()
    file(READ "${KATMED_APPSTORE_TARGET_REAL}" KATMED_APPSTORE_MAC48)
    string(STRIP "${KATMED_APPSTORE_MAC48}" KATMED_APPSTORE_MAC48)
    string(LENGTH "${KATMED_APPSTORE_MAC48}" KATMED_APPSTORE_MAC48_LENGTH)
    if(NOT KATMED_APPSTORE_MAC48_LENGTH EQUAL 12 OR
       NOT KATMED_APPSTORE_MAC48 MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "MAC48 build input must contain exactly twelve hexadecimal digits")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
    file(CHMOD "${CMAKE_BINARY_DIR}" "${CMAKE_BINARY_DIR}/generated"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    file(WRITE "${CMAKE_BINARY_DIR}/generated/katmed_appmaint_target.h"
        "#define KATMED_APPMAINT_TARGET_MAC48 UINT64_C(0x${KATMED_APPSTORE_MAC48})\n")
    file(CHMOD "${CMAKE_BINARY_DIR}/generated/katmed_appmaint_target.h"
        PERMISSIONS OWNER_READ OWNER_WRITE)
    unset(KATMED_APPSTORE_MAC48)
    set(PROJECT_VER "appmaint-r2-20261025")
endif()
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
idf_build_set_property(KATMED_APPSTORE_MAINTENANCE "${KATMED_APPSTORE_MAINTENANCE}")
idf_build_set_property(KATMED_ACME_RESOURCE_PROBE "${KATMED_ACME_RESOURCE_PROBE}")
idf_build_set_property(KATMED_ACME_RESOURCE_CLIENT "${KATMED_ACME_RESOURCE_CLIENT}")
idf_build_set_property(KATMED_ACME_RESOURCE_SERVER "${KATMED_ACME_RESOURCE_SERVER}")
set(DEPENDENCIES_LOCK "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.${IDF_TARGET}.lock")
idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK}")
if(KATMED_APPSTORE_MAINTENANCE)
    project(katmed_appmaint_c6)
else()
    project(katmed_foundation)
endif()
set(KATMED_USERFS_SEED_DIR "${CMAKE_BINARY_DIR}/katmed_userfs_seed")
set(KATMED_SEED_TARGET "c3")
if(IDF_TARGET STREQUAL "esp32c6")
    set(KATMED_SEED_TARGET "c6")
endif()
# Regenerate before every LittleFS build, including incremental asset edits.
add_custom_target(katmed_image_seed
    COMMAND /usr/bin/python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/prepare_image_seed.py"
            --target "${KATMED_SEED_TARGET}"
            --manifest "${CMAKE_CURRENT_SOURCE_DIR}/../assets/generated/manifest.json"
            --output "${KATMED_USERFS_SEED_DIR}"
    VERBATIM)
file(MAKE_DIRECTORY "${KATMED_USERFS_SEED_DIR}")
littlefs_create_partition_image(user_images "${KATMED_USERFS_SEED_DIR}"
    FLASH_IN_PROJECT DEPENDS katmed_image_seed)
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
