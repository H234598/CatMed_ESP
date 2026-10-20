cmake_minimum_required(VERSION 3.20)
project(katmed_host LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(katmed_core firmware/components/katmed_core/katmed_core.cpp)
target_include_directories(katmed_core PUBLIC firmware/components/katmed_core/include)
target_compile_options(katmed_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
enable_testing()
add_executable(check_core tests/check_core.cpp)
target_link_libraries(check_core PRIVATE katmed_core)
target_compile_options(check_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME core_contract COMMAND check_core)

add_executable(check_reset_workflow
    tests/check_reset_workflow.c
    firmware/components/katmed_reset/katmed_reset_journal.c)
target_include_directories(check_reset_workflow PRIVATE
    firmware/boards
    firmware/components/katmed_reset/include)
target_compile_options(check_reset_workflow PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME reset_workflow_contract COMMAND check_reset_workflow)

# Exercise the same bounded reader and LVGL/TJpgD adapter used by both firmware
# targets against the checked-in image packs. The generated index is target-local.
set(KATMED_IMAGE_COMPONENT "${CMAKE_CURRENT_SOURCE_DIR}/firmware/components/katmed_images")
set(KATMED_IMAGE_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets/generated")
set(KATMED_CATALOG_PYTHON "/usr/bin/python3")
set(KATMED_MBEDTLS_SOURCE "$ENV{IDF_PATH}/components/mbedtls/mbedtls" CACHE PATH
    "Pinned ESP-IDF mbedTLS source used by host and firmware")
if(NOT EXISTS "${KATMED_MBEDTLS_SOURCE}/include/mbedtls/ssl.h" OR
   NOT EXISTS "${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/include/mbedtls/md.h")
    message(FATAL_ERROR "Set IDF_PATH or KATMED_MBEDTLS_SOURCE to the pinned ESP-IDF mbedTLS source")
endif()
set(MBEDTLS_FRAMEWORK_DIR "${KATMED_MBEDTLS_SOURCE}/framework")
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
# Mirror ESP-IDF's component-level public include directory for the pinned port.
include_directories("${KATMED_MBEDTLS_SOURCE}/../port/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/build-c3/config")
add_subdirectory("${KATMED_MBEDTLS_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls" EXCLUDE_FROM_ALL)
target_include_directories(tfpsacrypto PUBLIC "${KATMED_MBEDTLS_SOURCE}/../port/include")
set(LV_BUILD_CONF_PATH "${KATMED_IMAGE_COMPONENT}/host/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
execute_process(
    COMMAND /usr/bin/python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_lvgl_compat.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/managed_components/lvgl__lvgl"
    RESULT_VARIABLE KATMED_LVGL_COMPAT_RESULT)
if(NOT KATMED_LVGL_COMPAT_RESULT EQUAL 0)
    message(FATAL_ERROR "Pinned LVGL TJpgD compatibility check failed")
endif()
add_subdirectory(firmware/managed_components/lvgl__lvgl "${CMAKE_CURRENT_BINARY_DIR}/lvgl")

add_custom_target(verify_image_assets
    COMMAND "${KATMED_CATALOG_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_images.py" --verify-only
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking current source selection, crops, JPEGs and packs"
    VERBATIM)
file(GLOB_RECURSE KATMED_JPEGS CONFIGURE_DEPENDS "${KATMED_IMAGE_ASSETS}/*.jpg")
foreach(board IN ITEMS c3 c6)
    set(catalog "${CMAKE_CURRENT_BINARY_DIR}/katmed_catalog_${board}.c")
    add_custom_command(OUTPUT "${catalog}"
        COMMAND "${KATMED_CATALOG_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_firmware_catalog.py"
                --target "${board}" --manifest "${KATMED_IMAGE_ASSETS}/manifest.json"
                --output "${catalog}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_firmware_catalog.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_images.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/crop_focus.json"
                "${KATMED_IMAGE_ASSETS}/manifest.json"
                "${KATMED_IMAGE_ASSETS}/c3_images.bin"
                "${KATMED_IMAGE_ASSETS}/c6_images.bin" ${KATMED_JPEGS}
        COMMENT "Generating validated ${board} image index"
        VERBATIM)
    add_executable(check_image_${board}
        tests/check_image_reader.cpp
        "${KATMED_IMAGE_COMPONENT}/katmed_images_reader.c"
        "${KATMED_IMAGE_COMPONENT}/katmed_images_lvgl.c"
        "${KATMED_IMAGE_COMPONENT}/katmed_images_hash.c"
        "${catalog}")
    target_include_directories(check_image_${board} PRIVATE
        "${KATMED_IMAGE_COMPONENT}/include" "${KATMED_IMAGE_COMPONENT}")
    # mbedTLS 4 splits public headers across these SDK directories. The host
    # decoder target needs all three for mbedtls/md.h and its private_access.h.
    target_include_directories(check_image_${board} SYSTEM PRIVATE
        "${KATMED_MBEDTLS_SOURCE}/include"
        "${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/include"
        "${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/drivers/builtin/include")
    target_link_libraries(check_image_${board} PRIVATE katmed_core lvgl::lvgl tfpsacrypto)
    target_link_options(check_image_${board} PRIVATE -Wl,--wrap=lv_strdup)
    target_compile_options(check_image_${board} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(check_image_${board} verify_image_assets)
    add_test(NAME image_${board}_contract COMMAND check_image_${board}
        "${KATMED_IMAGE_ASSETS}/${board}_images.bin")
endforeach()

option(KATMED_SANITIZERS "Check host memory and undefined behaviour" OFF)
if(KATMED_SANITIZERS)
    foreach(t katmed_core check_core)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
