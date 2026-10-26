#!/usr/bin/env bash
set -euo pipefail

repo="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
tool_dir="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
build="${KATMED_BOARD_CACHE_BUILD_DIR:-${repo}/build-board-cache-review}"
"${tool_dir}/bootstrap/bin/cmake" -S "${repo}/firmware/components/katmed_ui/host" \
    -B "${build}" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="${tool_dir}/bootstrap/bin/ninja" \
    -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++
"${tool_dir}/bootstrap/bin/cmake" --build "${build}" --target lvgl -j 8

flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-unused-variable
    -ffunction-sections -fdata-sections
    "-DLV_CONF_PATH=\"${repo}/firmware/components/katmed_images/host/lv_conf.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo}/tests/host_board_cache_review/stubs"
    "-I${repo}/tests/host_store/stubs"
    "-I${repo}/tests/host_main/stubs"
    "-I${repo}/firmware/boards"
    "-I${repo}/firmware/main"
    "-I${repo}/firmware/components/board/include"
    "-I${repo}/firmware/components/katmed_ui/include"
    "-I${repo}/firmware/components/katmed_config_bridge/include"
    "-I${repo}/firmware/components/katmed_web/include"
    "-I${repo}/firmware/components/katmed_core/include"
    "-I${repo}/firmware/components/katmed_images/include"
    "-I${repo}/firmware/components/katmed_store/include"
    "-I${repo}/firmware/components/katmed_security/include"
    "-I${repo}/firmware/components/katmed_import/include"
    "-I${repo}/firmware/components/katmed_acme/include"
    "-I${repo}/firmware/components/katmed_reset/include"
    "-I${repo}/firmware/components/katmed_mesh/include"
    "-I${repo}/firmware/components/katmed_time/include"
    "-I${repo}/firmware/managed_components/lvgl__lvgl")

for source in \
    firmware/components/board/board_indev_gate.c \
    firmware/components/board/board_geometry.c \
    firmware/components/board/board_protocol.c \
    firmware/components/katmed_ui/ui.c \
    firmware/components/katmed_ui/navigation.c \
    firmware/components/katmed_ui/clock_view.c \
    firmware/components/katmed_ui/morse.c \
    firmware/components/katmed_ui/i18n_catalog.c \
    firmware/components/katmed_ui/i18n_date.c \
    firmware/components/katmed_ui/katmed_font_10.c \
    firmware/components/katmed_ui/katmed_font_12.c \
    firmware/components/katmed_ui/katmed_font_14.c \
    firmware/components/katmed_ui/katmed_font_26.c \
    firmware/components/katmed_ui/katmed_font_i18n_12.c \
    firmware/components/katmed_ui/katmed_font_i18n_14.c; do
    object="${build}/$(basename "${source}" .c).o"
    /usr/bin/gcc "${flags[@]}" -c "${repo}/${source}" -o "${object}"
done
/usr/bin/g++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    "-I${repo}/firmware/components/katmed_core/include" \
    -c "${repo}/firmware/components/katmed_core/katmed_core.cpp" \
    -o "${build}/katmed_core.o"

objects=("${build}"/{board_indev_gate,board_geometry,board_protocol,ui,navigation,clock_view,morse,i18n_catalog,i18n_date,katmed_font_10,katmed_font_12,katmed_font_14,katmed_font_26,katmed_font_i18n_12,katmed_font_i18n_14,katmed_core}.o)
for target in 3 6; do
    /usr/bin/gcc "${flags[@]}" \
        -include "${repo}/tests/host_p7_integration/host_restart_decl.h" \
        -DCONFIG_IDF_TARGET_ESP32C${target}=1 \
        -c "${repo}/tests/host_board_cache_review/check_board_main_ui.c" \
        -o "${build}/review_c${target}.o"
    /usr/bin/g++ -Wl,--gc-sections "${build}/review_c${target}.o" \
        "${objects[@]}" "${build}/lvgl/liblvgl.a" -lm \
        -o "${build}/review_c${target}"
    if [[ "${KATMED_BOARD_CACHE_ONLY_BRIGHTNESS:-0}" != 1 ]]; then
        for mode in alarm shared local; do
            for fault in normal delayed_poll failed_lock; do
                timeout 20s "${build}/review_c${target}" "${mode}" "${fault}"
            done
        done
    fi
done
for target in 1 40 50 100; do
    timeout 20s "${build}/review_c3" brightness "${target}"
    timeout 20s "${build}/review_c6" brightness "${target}"
done
