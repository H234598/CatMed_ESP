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

add_executable(check_ui_surface
    "${ROOT}/tests/check_ui_surface.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp")
target_include_directories(check_ui_surface PRIVATE
    "${UI}/include"
    "${ROOT}/firmware/components/katmed_core/include"
    "${ROOT}/firmware/components/katmed_images/include")
target_compile_definitions(check_ui_surface PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_ui_surface PRIVATE -Wall -Wextra -Werror)
target_compile_features(check_ui_surface PRIVATE cxx_std_17)
target_link_libraries(check_ui_surface PRIVATE lvgl m)

add_executable(check_ui_morse
    "${ROOT}/tests/check_ui_morse.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp")
target_include_directories(check_ui_morse PRIVATE
    "${UI}/include"
    "${ROOT}/firmware/components/katmed_core/include"
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
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp")
target_include_directories(check_ui_interaction PRIVATE "${UI}/include"
    "${ROOT}/firmware/components/katmed_core/include"
    "${ROOT}/firmware/components/katmed_images/include")
target_compile_definitions(check_ui_interaction PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_ui_interaction PRIVATE -Wall -Wextra -Werror)
target_link_libraries(check_ui_interaction PRIVATE lvgl m)
enable_testing()
add_executable(check_settings_values
    "${ROOT}/tests/host_store/check_settings_values.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp"
    "${ROOT}/firmware/components/katmed_store/katmed_store.cpp"
    "${ROOT}/firmware/components/katmed_time/katmed_time.c")
target_include_directories(check_settings_values PRIVATE
    "${ROOT}/tests/host_store/stubs" "${ROOT}/tests/host_main/stubs"
    "${ROOT}/firmware/main" "${ROOT}/firmware/components/board/include"
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
