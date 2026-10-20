cmake_minimum_required(VERSION 3.16)
project(katmed_reset_ui_host LANGUAGES C CXX)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(UI "${ROOT}/firmware/components/katmed_ui")
set(LV_BUILD_CONF_PATH "${UI}/host/lv_conf_34k.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/firmware/managed_components/lvgl__lvgl" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
add_executable(check_reset_ui
    "${ROOT}/tests/check_ui_surface.c"
    "${UI}/navigation.c" "${UI}/clock_view.c" "${UI}/ui.c" "${UI}/morse.c"
    "${UI}/i18n_catalog.c" "${UI}/i18n_date.c"
    "${UI}/katmed_font_10.c" "${UI}/katmed_font_12.c"
    "${UI}/katmed_font_14.c" "${UI}/katmed_font_26.c"
    "${UI}/katmed_font_i18n_12.c" "${UI}/katmed_font_i18n_14.c"
    "${ROOT}/firmware/components/katmed_reset/katmed_reset_journal.c"
    "${ROOT}/firmware/components/katmed_core/katmed_core.cpp")
target_include_directories(check_reset_ui PRIVATE
    "${ROOT}/firmware/boards" "${UI}/include"
    "${ROOT}/firmware/components/katmed_core/include"
    "${ROOT}/firmware/components/katmed_reset/include"
    "${ROOT}/firmware/components/katmed_images/include")
target_compile_definitions(check_reset_ui PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(check_reset_ui PRIVATE -Wall -Wextra -Werror)
target_compile_features(check_reset_ui PRIVATE cxx_std_17)
target_link_libraries(check_reset_ui PRIVATE lvgl m)
