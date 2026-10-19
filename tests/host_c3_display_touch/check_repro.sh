#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
build="${repo}/build-ui-host"
tools="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
"${tools}/bootstrap/bin/cmake" -S "${repo}/firmware/components/katmed_ui/host" -B "${build}" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="${tools}/bootstrap/bin/ninja" \
    -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ >/dev/null
"${tools}/bootstrap/bin/cmake" --build "${build}" --target check_ui_surface -j 8 >/dev/null
flags=(-std=c11 -Wall -Wextra -Werror -Wno-unused-variable -ffunction-sections -fdata-sections
    "-DLV_CONF_PATH=\"${repo}/firmware/components/katmed_images/host/lv_conf.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo}/tests/host_store/stubs" "-I${repo}/tests/host_main/stubs"
    "-I${repo}/firmware/components/board/include" "-I${repo}/firmware/components/katmed_ui/include"
    "-I${repo}/firmware/components/katmed_config_bridge/include"
    "-I${repo}/firmware/components/katmed_web/include"
    "-I${repo}/firmware/components/katmed_core/include" "-I${repo}/firmware/components/katmed_store/include"
    "-I${repo}/firmware/components/katmed_mesh/include" "-I${repo}/firmware/components/katmed_time/include"
    "-I${repo}/firmware/components/katmed_images/include" "-I${repo}/firmware/main"
    "-I${repo}/firmware/managed_components/lvgl__lvgl")
mkdir -p "${build}/c3-repro"
cc "${flags[@]}" -c "${repo}/firmware/components/katmed_ui/ui.c" -o "${build}/c3-repro/ui.o"
cc "${flags[@]}" -c "${repo}/firmware/components/board/board_touch_buffer.c" -o "${build}/c3-repro/board_touch_buffer.o"
cc "${flags[@]}" -c "${repo}/tests/host_c3_display_touch/repro.c" -o "${build}/c3-repro/repro.o"
obj="${build}/CMakeFiles/check_ui_surface.dir${repo}/firmware/components"
objects=("${obj}/katmed_ui/navigation.c.o" "${obj}/katmed_ui/clock_view.c.o"
    "${obj}/katmed_ui/katmed_font_10.c.o" "${obj}/katmed_ui/katmed_font_12.c.o"
    "${obj}/katmed_ui/katmed_font_14.c.o" "${obj}/katmed_ui/katmed_font_26.c.o"
    "${obj}/katmed_core/katmed_core.cpp.o")
g++ -Wl,--gc-sections "${build}/c3-repro/repro.o" "${build}/c3-repro/ui.o" "${build}/c3-repro/board_touch_buffer.o" \
    "${objects[@]}" "${build}/lvgl/lib/liblvgl.a" -lm -o "${build}/c3-repro/repro"
if (( $# == 0 )); then set -- lock_tap lock_swipe old_round_tap overflow bar_pixels fallback; fi
for mode in "$@"; do
    "${build}/c3-repro/repro" "$mode"
done
