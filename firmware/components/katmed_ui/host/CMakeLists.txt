cmake_minimum_required(VERSION 3.16)
project(katmed_ui_host LANGUAGES C CXX)

set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../")
set(UI "${CMAKE_CURRENT_LIST_DIR}/..")
option(KATMED_UI_HOST_MEM34 "Exercise the firmware's 34 KiB LVGL pool" OFF)
if(KATMED_UI_HOST_MEM34)
    set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf_34k.h" CACHE PATH "" FORCE)
else()
    set(LV_BUILD_CONF_PATH "${ROOT}/firmware/components/katmed_images/host/lv_conf.h"
        CACHE PATH "" FORCE)
endif()
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/firmware/managed_components/lvgl__lvgl" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
set(KATMED_I18N_FONT_SOURCES
    "${UI}/katmed_font_i18n_12.c" "${UI}/katmed_font_i18n_14.c")

add_executable(check_ui_surface
    "${ROOT}/tests/check_ui_surface.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c" "${UI}/i18n_catalog.c" "${UI}/i18n_date.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    ${KATMED_I18N_FONT_SOURCES}
    "${ROOT}/firmware/components/katmed_reset/katmed_reset_journal.c"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp")
target_include_directories(check_ui_surface PRIVATE
    "${ROOT}/firmware/boards"
    "${UI}/include"
    "${ROOT}/firmware/components/katmed_core/include"
    "${ROOT}/firmware/components/katmed_reset/include"
    "${ROOT}/firmware/components/katmed_images/include")
target_compile_definitions(check_ui_surface PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_ui_surface PRIVATE -Wall -Wextra -Werror)
target_compile_features(check_ui_surface PRIVATE cxx_std_17)
target_link_libraries(check_ui_surface PRIVATE lvgl m)

add_executable(check_ui_morse
    "${ROOT}/tests/check_ui_morse.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c" "${UI}/i18n_catalog.c" "${UI}/i18n_date.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    ${KATMED_I18N_FONT_SOURCES}
    "${ROOT}/firmware/components/katmed_reset/katmed_reset_journal.c"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp")
target_include_directories(check_ui_morse PRIVATE
    "${ROOT}/firmware/boards"
    "${UI}/include"
    "${ROOT}/firmware/components/katmed_core/include"
    "${ROOT}/firmware/components/katmed_reset/include"
    "${ROOT}/firmware/components/katmed_images/include")
target_compile_definitions(check_ui_morse PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_ui_morse PRIVATE -Wall -Wextra -Werror)
target_compile_features(check_ui_morse PRIVATE cxx_std_17)
target_link_libraries(check_ui_morse PRIVATE lvgl m)

add_executable(check_morse_engine
    "${ROOT}/tests/check_morse_engine.c" "${UI}/morse.c")
target_include_directories(check_morse_engine PRIVATE "${UI}/include")
target_compile_options(check_morse_engine PRIVATE -Wall -Wextra -Werror)

add_executable(check_ui_navigation
    "${ROOT}/tests/check_ui_navigation.c" "${UI}/navigation.c")
target_include_directories(check_ui_navigation PRIVATE
    "${UI}/include" "${ROOT}/firmware/components/katmed_core/include")
target_compile_options(check_ui_navigation PRIVATE -Wall -Wextra -Werror)

add_executable(check_ui_interaction
    "${ROOT}/tests/check_ui_interaction.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c" "${UI}/i18n_catalog.c" "${UI}/i18n_date.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    ${KATMED_I18N_FONT_SOURCES}
    "${ROOT}/firmware/components/katmed_reset/katmed_reset_journal.c"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp")
target_include_directories(check_ui_interaction PRIVATE "${UI}/include"
    "${ROOT}/firmware/boards"
    "${ROOT}/firmware/components/katmed_core/include"
    "${ROOT}/firmware/components/katmed_reset/include"
    "${ROOT}/firmware/components/katmed_images/include")
target_compile_definitions(check_ui_interaction PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_ui_interaction PRIVATE -Wall -Wextra -Werror)
target_link_libraries(check_ui_interaction PRIVATE lvgl m)
enable_testing()
add_executable(check_settings_values
    "${ROOT}/tests/host_store/check_settings_values.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c" "${UI}/i18n_catalog.c" "${UI}/i18n_date.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    ${KATMED_I18N_FONT_SOURCES}
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp"
    "${ROOT}/firmware/components/katmed_store/katmed_store.cpp"
    "${ROOT}/firmware/components/katmed_time/katmed_time.c")
target_include_directories(check_settings_values PRIVATE
    "${ROOT}/tests/host_store/stubs" "${ROOT}/tests/host_main/stubs"
    "${ROOT}/firmware/components/katmed_config_bridge/include"
    "${ROOT}/firmware/components/katmed_web/include"
    "${ROOT}/firmware/main" "${ROOT}/firmware/components/board/include"
    "${ROOT}/firmware/components/katmed_reset/include"
    "${ROOT}/firmware/components/katmed_acme/include"
    "${ROOT}/firmware/components/katmed_security/include"
    "${ROOT}/firmware/components/katmed_import/include"
    "${UI}/include" "${ROOT}/firmware/components/katmed_core/include"
    "${ROOT}/firmware/components/katmed_store/include"
    "${ROOT}/firmware/components/katmed_time/include"
    "${ROOT}/firmware/components/katmed_mesh/include"
    "${ROOT}/firmware/components/katmed_images/include")
target_compile_definitions(check_settings_values PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_settings_values PRIVATE -Wall -Wextra -Werror
    -Wno-unused-variable -ffunction-sections -fdata-sections)
target_compile_features(check_settings_values PRIVATE cxx_std_17)
target_link_options(check_settings_values PRIVATE -Wl,--gc-sections)
target_link_libraries(check_settings_values PRIVATE lvgl m z)
foreach(format IN ITEMS "240;240" "172;320" "320;172")
    list(GET format 0 width)
    list(GET format 1 height)
    add_test(NAME settings_values_${width}x${height}
        COMMAND check_settings_values ${width} ${height})
endforeach()
add_test(NAME ui_navigation COMMAND check_ui_navigation)
add_test(NAME local_morse COMMAND check_morse_engine)
add_test(NAME ui_seltsam COMMAND check_ui_morse)
foreach(format IN ITEMS "240;240" "172;320" "320;172")
    list(GET format 0 width)
    list(GET format 1 height)
    add_test(NAME interaction_${width}x${height} COMMAND check_ui_interaction ${width} ${height})
endforeach()

# Real UI/Main/Store and original image decoder regression; pinned SDK only.
set(KATMED_MBEDTLS_SOURCE "$ENV{IDF_PATH}/components/mbedtls/mbedtls" CACHE PATH "Pinned SDK mbedTLS")
set(MBEDTLS_FRAMEWORK_DIR "${KATMED_MBEDTLS_SOURCE}/framework")
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
include_directories("${KATMED_MBEDTLS_SOURCE}/../port/include"
    "${ROOT}/firmware/build-c3/config")
add_subdirectory("${KATMED_MBEDTLS_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls" EXCLUDE_FROM_ALL)
target_include_directories(tfpsacrypto PUBLIC "${KATMED_MBEDTLS_SOURCE}/../port/include")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/refinement_catalog.c"
    COMMAND /usr/bin/python3 "${ROOT}/tools/generate_firmware_catalog.py"
        --target c3 --output "${CMAKE_CURRENT_BINARY_DIR}/refinement_catalog.c"
    DEPENDS "${ROOT}/assets/generated/manifest.json" "${ROOT}/assets/generated/c3_images.bin")
add_executable(check_ui_refinement
    "${ROOT}/tests/check_ui_refinement.c"
    "${ROOT}/tests/check_ui_refinement_main.c"
    "${ROOT}/firmware/components/katmed_images/katmed_images_reader.c"
    "${ROOT}/firmware/components/katmed_images/katmed_images_lvgl.c"
    "${ROOT}/firmware/components/katmed_images/katmed_images_hash.c"
    "${ROOT}/firmware/components/katmed_reset/katmed_reset_journal.c"
    "${CMAKE_CURRENT_BINARY_DIR}/refinement_catalog.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c" "${UI}/i18n_catalog.c" "${UI}/i18n_date.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    ${KATMED_I18N_FONT_SOURCES}
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp"
    "${ROOT}/firmware/components/katmed_store/katmed_store.cpp"
    "${ROOT}/firmware/components/katmed_time/katmed_time.c")
target_include_directories(check_ui_refinement PRIVATE
    "${ROOT}/tests/host_store/stubs" "${ROOT}/tests/host_main/stubs"
    "${ROOT}/firmware/boards"
    "${ROOT}/firmware/components/katmed_config_bridge/include"
    "${ROOT}/firmware/components/katmed_web/include"
    "${ROOT}/firmware/main" "${ROOT}/firmware/components/board/include"
    "${ROOT}/firmware/components/katmed_reset/include"
    "${ROOT}/firmware/components/katmed_acme/include"
    "${ROOT}/firmware/components/katmed_security/include"
    "${ROOT}/firmware/components/katmed_import/include"
    "${UI}/include" "${ROOT}/firmware/components/katmed_core/include"
    "${ROOT}/firmware/components/katmed_store/include"
    "${ROOT}/firmware/components/katmed_time/include"
    "${ROOT}/firmware/components/katmed_mesh/include"
    "${ROOT}/firmware/components/katmed_images/include"
    "${ROOT}/firmware/components/katmed_images")
target_include_directories(check_ui_refinement SYSTEM PRIVATE
    "${KATMED_MBEDTLS_SOURCE}/include"
    "${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/include"
    "${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/drivers/builtin/include")
target_compile_definitions(check_ui_refinement PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_ui_refinement PRIVATE -Wall -Wextra -Werror
    -Wno-unused-variable -ffunction-sections -fdata-sections)
target_compile_features(check_ui_refinement PRIVATE cxx_std_17)
target_link_options(check_ui_refinement PRIVATE -Wl,--gc-sections)
target_link_libraries(check_ui_refinement PRIVATE lvgl m z tfpsacrypto)
foreach(mode IN ITEMS random save nav scroll scroll-status indices clock-menu tick)
    add_test(NAME ui_refinement_${mode} COMMAND check_ui_refinement ${mode}
        "${ROOT}/assets/generated/c3_images.bin")
endforeach()
foreach(format IN ITEMS "240;240" "172;320" "320;172")
    list(GET format 0 width)
    list(GET format 1 height)
    add_test(NAME ui_refinement_clock_${width}x${height} COMMAND check_ui_refinement clock
        "${ROOT}/assets/generated/c3_images.bin" ${width} ${height})
endforeach()

# Real coordinator binding for the production slideshow producer; SDK stubs only.
add_executable(check_ui_refinement_mesh
    "${ROOT}/tests/check_ui_refinement_tick.c"
    "${ROOT}/tests/check_ui_refinement_mesh.cpp"
    "${ROOT}/firmware/components/katmed_names/katmed_names.cpp"
    "${ROOT}/firmware/components/katmed_names/katmed_names_wire.cpp"
    "${ROOT}/firmware/components/katmed_mesh/katmed_mesh_wire.cpp"
    "${ROOT}/firmware/components/katmed_mesh/katmed_mesh_payload.cpp"
    "${ROOT}/firmware/components/katmed_mesh/katmed_mesh_status.cpp"
    "${ROOT}/firmware/components/katmed_mesh/katmed_mesh_session.cpp"
    "${ROOT}/firmware/components/katmed_mesh/katmed_mesh_idf.cpp"
    "${ROOT}/firmware/components/katmed_store/katmed_store.cpp"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp"
    "${ROOT}/firmware/components/katmed_time/katmed_time.c")
get_target_property(refinement_includes check_ui_refinement INCLUDE_DIRECTORIES)
target_include_directories(check_ui_refinement_mesh PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${ROOT}/tests/host_mesh/stubs>"
    "${ROOT}/firmware/components/katmed_names/include" ${refinement_includes})
target_compile_definitions(check_ui_refinement_mesh PRIVATE KATMED_MESH_HOST LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_ui_refinement_mesh PRIVATE -Wall -Wextra -Werror
    -Wno-unused-function -Wno-unused-variable -ffunction-sections -fdata-sections)
target_compile_features(check_ui_refinement_mesh PRIVATE cxx_std_17)
target_link_options(check_ui_refinement_mesh PRIVATE -Wl,--gc-sections)
target_link_libraries(check_ui_refinement_mesh PRIVATE lvgl m z -l:libcrypto.so.3)
add_test(NAME ui_refinement_real_mesh COMMAND check_ui_refinement_mesh)

# The HHMM-only keyboard uses the existing real UI/Main/Core/Store fixture.
get_target_property(hhmm_sources check_ui_refinement SOURCES)
list(REMOVE_ITEM hhmm_sources "${ROOT}/tests/check_ui_refinement.c")
add_executable(check_ui_hhmm "${ROOT}/tests/check_ui_hhmm.c" ${hhmm_sources})
get_target_property(hhmm_includes check_ui_refinement INCLUDE_DIRECTORIES)
target_include_directories(check_ui_hhmm PRIVATE ${hhmm_includes})
target_compile_definitions(check_ui_hhmm PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_ui_hhmm PRIVATE -Wall -Wextra -Werror
    -Wno-unused-variable -ffunction-sections -fdata-sections)
target_compile_features(check_ui_hhmm PRIVATE cxx_std_17)
target_link_options(check_ui_hhmm PRIVATE -Wl,--gc-sections)
target_link_libraries(check_ui_hhmm PRIVATE lvgl m z tfpsacrypto)
foreach(format IN ITEMS "240;240" "172;320" "320;172")
    list(GET format 0 width)
    list(GET format 1 height)
    foreach(mode IN ITEMS layout editing flow decimal)
        add_test(NAME hhmm_${mode}_${width}x${height} COMMAND check_ui_hhmm ${mode}
            "${ROOT}/assets/generated/c3_images.bin" ${width} ${height})
    endforeach()
endforeach()
