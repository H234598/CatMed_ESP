#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="$repo/build-ui-host"
tools="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
"$tools/bootstrap/bin/cmake" -S "$repo/firmware/components/katmed_ui/host" \
    -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$tools/bootstrap/bin/ninja" \
    -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ >/dev/null
"$tools/bootstrap/bin/cmake" --build "$build" --target check_ui_surface -j 8 >/dev/null
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
flags=(-std=c11 -Wall -Wextra -Werror -Wno-unused-variable
    -ffunction-sections -fdata-sections
    "-DLV_CONF_PATH=\"$repo/firmware/components/katmed_images/host/lv_conf.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I$repo/tests/host_store/stubs" "-I$repo/tests/host_main/stubs"
    "-I$repo/firmware/components/board/include"
    "-I$repo/firmware/components/katmed_ui/include"
    "-I$repo/firmware/components/katmed_config_bridge/include"
    "-I$repo/firmware/components/katmed_web/include"
    "-I$repo/firmware/components/katmed_core/include"
    "-I$repo/firmware/components/katmed_store/include"
    "-I$repo/firmware/components/katmed_mesh/include"
    "-I$repo/firmware/components/katmed_time/include"
    "-I$repo/firmware/components/katmed_images/include"
    "-I$repo/firmware/main"
    "-I$repo/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "$repo/tests/host_p7_integration/check_touch_store.c" -o "$out/repro.o"
cc "${flags[@]}" -c "$repo/firmware/components/katmed_ui/ui.c" -o "$out/ui.o"
cc "${flags[@]}" -c "$repo/firmware/components/board/board_touch_buffer.c" -o "$out/buffer.o"
cc "${flags[@]}" -c "$repo/firmware/components/katmed_time/katmed_time.c" -o "$out/time.o"
c++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo/firmware/components/katmed_core/include" \
    -I"$repo/firmware/components/katmed_store/include" \
    -c "$repo/firmware/components/katmed_store/katmed_store.cpp" -o "$out/store.o"
objects="$build/CMakeFiles/check_ui_surface.dir$repo/firmware/components"
g++ -Wl,--gc-sections "$out/repro.o" "$out/ui.o" "$out/buffer.o" \
    "$out/time.o" "$out/store.o" \
    "$objects/katmed_ui/navigation.c.o" "$objects/katmed_ui/clock_view.c.o" \
    "$objects/katmed_ui/katmed_font_10.c.o" "$objects/katmed_ui/katmed_font_12.c.o" \
    "$objects/katmed_ui/katmed_font_14.c.o" "$objects/katmed_ui/katmed_font_26.c.o" \
    "$objects/katmed_core/katmed_core.cpp.o" "$build/lvgl/lib/liblvgl.a" \
    -lm -lz -o "$out/repro"
ulimit -c 0
"$out/repro"
