#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$repo_root/tools/idf_env.sh"
tool_dir="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
build_dir="$repo_root/build-ui-host-32"
out_dir="$(mktemp -d -p /tmp katmed-p7-ui-review-XXXXXX)"
trap 'rm -rf "$out_dir"' EXIT
"$tool_dir/bootstrap/bin/cmake" -S "$repo_root/firmware/components/katmed_ui/host" \
  -B "$build_dir" -G Ninja -DCMAKE_MAKE_PROGRAM="$tool_dir/bootstrap/bin/ninja" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DKATMED_UI_HOST_MEM34=ON > /dev/null
"$tool_dir/bootstrap/bin/cmake" --build "$build_dir" --target check_ui_surface -j 8 > /dev/null
flags=(-std=c11 -Wall -Wextra -Werror
  "-DLV_CONF_PATH=\"$repo_root/firmware/components/katmed_ui/host/lv_conf_34k.h\""
  -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
  "-I$repo_root/firmware/components/katmed_ui/include"
  "-I$repo_root/firmware/components/katmed_core/include"
  "-I$repo_root/firmware/components/katmed_images/include"
  "-I$repo_root/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "$repo_root/tests/host_p7_review/check_alarm_feedback.c" \
  -o "$out_dir/check_alarm_feedback.o"
ui_objects_root="$build_dir/CMakeFiles/check_ui_surface.dir$repo_root/firmware/components"
ui_objects=(
  "$ui_objects_root/katmed_ui/navigation.c.o"
  "$ui_objects_root/katmed_ui/clock_view.c.o"
  "$ui_objects_root/katmed_ui/ui.c.o"
  "$ui_objects_root/katmed_ui/katmed_font_10.c.o"
  "$ui_objects_root/katmed_ui/katmed_font_12.c.o"
  "$ui_objects_root/katmed_ui/katmed_font_14.c.o"
  "$ui_objects_root/katmed_ui/katmed_font_26.c.o"
  "$ui_objects_root/katmed_core/katmed_core.cpp.o")
g++ "$out_dir/check_alarm_feedback.o" "${ui_objects[@]}" \
  "$build_dir/lvgl/lib/liblvgl.a" -lm -o "$out_dir/check_alarm_feedback"
timeout 20s "$out_dir/check_alarm_feedback"
