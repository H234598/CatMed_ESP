cmake_minimum_required(VERSION 3.16)
project(katmed_image_host_checks LANGUAGES C CXX)

set(KATMED_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../")
set(KATMED_COMPONENT "${CMAKE_CURRENT_LIST_DIR}/..")
set(KATMED_MBEDTLS_SOURCE "$ENV{IDF_PATH}/components/mbedtls/mbedtls" CACHE PATH
    "Pinned ESP-IDF mbedTLS source used by both host and target")
if(NOT EXISTS "${KATMED_MBEDTLS_SOURCE}/include/mbedtls/ssl.h" OR
   NOT EXISTS "${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/include/mbedtls/md.h")
    message(FATAL_ERROR "Set IDF_PATH or KATMED_MBEDTLS_SOURCE to the pinned ESP-IDF mbedTLS source")
endif()
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Warnings in pinned SDK code are reported" FORCE)
set(Python3_EXECUTABLE "/usr/bin/python3" CACHE FILEPATH "Image preparation Python with Pillow" FORCE)
add_subdirectory("${KATMED_MBEDTLS_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls" EXCLUDE_FROM_ALL)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${KATMED_ROOT}/tools/prepare_lvgl_compat.py"
            "${KATMED_ROOT}/firmware/managed_components/lvgl__lvgl"
    RESULT_VARIABLE KATMED_LVGL_COMPAT_RESULT)
if(NOT KATMED_LVGL_COMPAT_RESULT EQUAL 0)
    message(FATAL_ERROR "Pinned LVGL TJpgD compatibility preparation failed")
endif()
add_subdirectory("${KATMED_ROOT}/firmware/managed_components/lvgl__lvgl" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
set(Python3_EXECUTABLE "/usr/bin/python3" CACHE FILEPATH "Image preparation Python with Pillow" FORCE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Verify originals and all derived files even when the generated C index is
# already up to date. Missing/changed inputs must not hide behind a warm build.
add_custom_target(verify_image_assets
    COMMAND "${Python3_EXECUTABLE}" "${KATMED_ROOT}/tools/prepare_images.py" --verify-only
    WORKING_DIRECTORY "${KATMED_ROOT}"
    VERBATIM)

foreach(target c3 c6)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}/katmed_catalog_generated.c")
    add_custom_command(
        OUTPUT "${generated}"
        COMMAND "${Python3_EXECUTABLE}" "${KATMED_ROOT}/tools/generate_firmware_catalog.py"
                --target "${target}" --output "${generated}"
        DEPENDS "${KATMED_ROOT}/tools/generate_firmware_catalog.py"
                "${KATMED_ROOT}/tools/prepare_images.py"
                "${KATMED_ROOT}/assets/crop_focus.json"
                "${KATMED_ROOT}/assets/generated/manifest.json"
                "${KATMED_ROOT}/assets/generated/c3_images.bin"
                "${KATMED_ROOT}/assets/generated/c6_images.bin"
        VERBATIM)
    add_executable(check_image_reader_${target}
        "${KATMED_ROOT}/tests/check_image_reader.cpp"
        "${KATMED_COMPONENT}/katmed_images_reader.c"
        "${KATMED_COMPONENT}/katmed_images_lvgl.c"
        "${KATMED_COMPONENT}/katmed_images_hash.c"
        "${generated}"
        "${KATMED_ROOT}/firmware/components/katmed_core/katmed_core.cpp")
    target_include_directories(check_image_reader_${target} PRIVATE
        "${KATMED_COMPONENT}" "${KATMED_COMPONENT}/include"
        "${KATMED_ROOT}/firmware/components/katmed_core/include")
    target_compile_features(check_image_reader_${target} PRIVATE cxx_std_17)
    target_compile_options(check_image_reader_${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_options(check_image_reader_${target} PRIVATE -Wl,--wrap=lv_strdup)
    target_link_libraries(check_image_reader_${target} PRIVATE lvgl mbedcrypto)
    add_dependencies(check_image_reader_${target} verify_image_assets)
endforeach()
