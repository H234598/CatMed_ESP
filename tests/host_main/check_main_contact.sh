#!/usr/bin/env bash
# Exercise the real main.c touch dispatch when an LVGL lock drops a sample.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
tool_dir="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
cmake_bin="${tool_dir}/bootstrap/bin/cmake"
ninja_bin="${tool_dir}/bootstrap/bin/ninja"
build_dir="${repo_dir}/build-ui-host"
out_dir="${build_dir}/main-contact"

"${cmake_bin}" -S "${repo_dir}/firmware/components/katmed_ui/host" \
    -B "${build_dir}" -G Ninja -DCMAKE_MAKE_PROGRAM="${ninja_bin}" \
    -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++
"${cmake_bin}" --build "${build_dir}" --target check_ui_surface -j 8
mkdir -p "${out_dir}"

flags=(-std=c11 -Wall -Wextra -Werror -Wno-unused-variable
    -ffunction-sections -fdata-sections
    "-DLV_CONF_PATH=\"${repo_dir}/firmware/components/katmed_images/host/lv_conf.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo_dir}/tests/host_store/stubs"
    "-I${repo_dir}/tests/host_main/stubs"
    "-I${repo_dir}/firmware/components/board/include"
    "-I${repo_dir}/firmware/components/katmed_ui/include"
    "-I${repo_dir}/firmware/components/katmed_config_bridge/include"
    "-I${repo_dir}/firmware/components/katmed_web/include"
    "-I${repo_dir}/firmware/components/katmed_core/include"
    "-I${repo_dir}/firmware/components/katmed_store/include"
    "-I${repo_dir}/firmware/components/katmed_mesh/include"
    "-I${repo_dir}/firmware/components/katmed_time/include"
    "-I${repo_dir}/firmware/components/katmed_images/include"
    "-I${repo_dir}/firmware/main"
    "-I${repo_dir}/firmware/components/katmed_reset/include"
    "-I${repo_dir}/firmware/components/katmed_acme/include"
    "-I${repo_dir}/firmware/components/katmed_security/include"
    "-I${repo_dir}/firmware/components/katmed_import/include"
    "-I${repo_dir}/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "${repo_dir}/firmware/components/katmed_ui/ui.c" -o "${out_dir}/ui.o"
cc "${flags[@]}" -c "${repo_dir}/tests/host_main/main_contact_regression.c" \
    -o "${out_dir}/main_contact_regression.o"

object_root="${build_dir}/CMakeFiles/check_ui_surface.dir${repo_dir}/firmware/components"
objects=(
    "${object_root}/katmed_ui/morse.c.o"
    "${object_root}/katmed_ui/navigation.c.o"
    "${object_root}/katmed_ui/clock_view.c.o"
    "${object_root}/katmed_ui/i18n_catalog.c.o"
    "${object_root}/katmed_ui/i18n_date.c.o"
    "${object_root}/katmed_ui/katmed_font_10.c.o"
    "${object_root}/katmed_ui/katmed_font_12.c.o"
    "${object_root}/katmed_ui/katmed_font_14.c.o"
    "${object_root}/katmed_ui/katmed_font_26.c.o"
    "${object_root}/katmed_ui/katmed_font_i18n_12.c.o"
    "${object_root}/katmed_ui/katmed_font_i18n_14.c.o"
    "${object_root}/katmed_core/katmed_core.cpp.o")
g++ -Wl,--gc-sections "${out_dir}/main_contact_regression.o" "${out_dir}/ui.o" \
    "${objects[@]}" "${build_dir}/lvgl/liblvgl.a" -lm -o "${out_dir}/check_main_contact"
"${out_dir}/check_main_contact" night_wake
timeout 20s "${out_dir}/check_main_contact" lost_error
timeout 20s "${out_dir}/check_main_contact" lost_move
timeout 20s "${out_dir}/check_main_contact" round_switch
timeout 20s "${out_dir}/check_main_contact" round_switch_due

"${out_dir}/check_main_contact" interaction_queue

"${out_dir}/check_main_contact" interaction_overflow

# The actual transient engine, not the recording stub, must accept this queue.
cc "${flags[@]}" -DKATMED_REAL_INTERACTION_ADAPTER -c \
    "${repo_dir}/tests/host_main/main_contact_regression.c" -o "${out_dir}/main-real.o"
g++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"${repo_dir}/firmware/components/katmed_core/include" \
    -I"${repo_dir}/firmware/components/katmed_mesh/include" \
    -c "${repo_dir}/tests/host_main/interaction_adapter.cpp" -o "${out_dir}/interaction-adapter.o"
g++ -Wl,--gc-sections "${out_dir}/main-real.o" "${out_dir}/interaction-adapter.o" \
    "${out_dir}/ui.o" "${objects[@]}" "${build_dir}/lvgl/liblvgl.a" -lm \
    -o "${out_dir}/check_main_real_adapter"
"${out_dir}/check_main_real_adapter" interaction_real_adapter

"${out_dir}/check_main_real_adapter" interaction_real_overflow
