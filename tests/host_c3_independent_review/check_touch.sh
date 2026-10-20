#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "${1:-}" in
  fix) source_path="$repo_root/firmware/main/main.c" ;;
  original)
    temp_source="$(mktemp --suffix=.c)"
    git -C "$repo_root" show d941faee:firmware/main/main.c > "$temp_source"
    source_path="$temp_source"
    ;;
  *) echo 'usage: check_touch.sh fix|original short_tap|swipe|overflow' >&2; exit 2 ;;
esac
case "${2:-}" in short_tap|swipe|overflow) ;; *) exit 2 ;; esac
build="$repo_root/build-ui-host"
tools_dir="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
"$tools_dir/bootstrap/bin/cmake" -S "$repo_root/firmware/components/katmed_ui/host" \
  -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$tools_dir/bootstrap/bin/ninja" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ >/dev/null
"$tools_dir/bootstrap/bin/cmake" --build "$build" --target check_ui_surface -j 8 >/dev/null
out="$(mktemp -d)"
trap 'rm -rf "$out" "${temp_source:-}"' EXIT
flags=(-std=c11 -Wall -Wextra -Werror -Wno-unused-variable
  -ffunction-sections -fdata-sections
  "-DLV_CONF_PATH=\"$repo_root/firmware/components/katmed_images/host/lv_conf.h\""
  -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
  "-I$repo_root/tests/host_store/stubs" "-I$repo_root/tests/host_main/stubs"
  "-I$repo_root/firmware/components/board/include"
  "-I$repo_root/firmware/components/katmed_ui/include"
  "-I$repo_root/firmware/components/katmed_config_bridge/include"
  "-I$repo_root/firmware/components/katmed_web/include"
  "-I$repo_root/firmware/components/katmed_reset/include"
  "-I$repo_root/firmware/components/katmed_acme/include"
  "-I$repo_root/firmware/components/katmed_core/include"
  "-I$repo_root/firmware/components/katmed_store/include"
  "-I$repo_root/firmware/components/katmed_mesh/include"
  "-I$repo_root/firmware/components/katmed_time/include"
  "-I$repo_root/firmware/components/katmed_images/include"
  "-I$repo_root/firmware/main"
  "-I$repo_root/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "$repo_root/firmware/components/katmed_ui/ui.c" -o "$out/ui.o"
cc "${flags[@]}" -c "$repo_root/firmware/components/board/board_touch_buffer.c" -o "$out/buffer.o"
cc "${flags[@]}" -DKATMED_MAIN_SOURCE="\"$source_path\"" \
  -c "$repo_root/tests/host_c3_independent_review/touch_path.c" -o "$out/repro.o"
objects="$build/CMakeFiles/check_ui_surface.dir$repo_root/firmware/components"
g++ -Wl,--gc-sections "$out/repro.o" "$out/ui.o" "$out/buffer.o" \
  "$objects/katmed_ui/navigation.c.o" "$objects/katmed_ui/clock_view.c.o" \
  "$objects/katmed_ui/katmed_font_10.c.o" "$objects/katmed_ui/katmed_font_12.c.o" \
  "$objects/katmed_ui/katmed_font_14.c.o" "$objects/katmed_ui/katmed_font_26.c.o" \
  "$objects/katmed_ui/morse.c.o" "$objects/katmed_core/katmed_core.cpp.o" \
  "$build/lvgl/liblvgl.a" \
  -lm -o "$out/repro"
ulimit -c 0
"$out/repro" "$2"
