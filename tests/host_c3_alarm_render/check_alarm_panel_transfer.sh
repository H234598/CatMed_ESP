#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
if [[ -z "${IDF_PATH:-}" ]]; then
    source "${repo_dir}/tools/idf_env.sh"
fi
tool_dir="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
build_dir="${1:?taskprivate-build-directory-required}"
ui_override="${2:-}"
bash "${repo_dir}/tests/host_c3_alarm_render/check_jd9853_transfer.sh" "${build_dir}"
"${tool_dir}/bootstrap/bin/cmake" -S "${repo_dir}/firmware/components/katmed_ui/host" \
    -B "${build_dir}" -G Ninja -DCMAKE_MAKE_PROGRAM="${tool_dir}/bootstrap/bin/ninja" \
    -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
    -DKATMED_UI_HOST_LVGL_SOURCE="${KATMED_HOST_LVGL_SOURCE:-${repo_dir}/firmware/build-a61-c3/katmed-overrides/lvgl__lvgl}" \
    -DKATMED_HOST_CONFIG_DIR="${KATMED_HOST_CONFIG_DIR:-${repo_dir}/firmware/build-a61-c3/config}" \
    -DKATMED_UI_HOST_MEM34=ON >/dev/null
"${tool_dir}/bootstrap/bin/cmake" --build "${build_dir}" --target check_ui_surface -j 8 >/dev/null
cc -std=c11 -Wall -Wextra -Werror \
    "-DLV_CONF_PATH=\"${repo_dir}/firmware/components/katmed_ui/host/lv_conf_34k.h\"" \
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE \
    -DKATMED_SURFACE_ALLOC_ACCOUNTING=1 \
    -I"${repo_dir}/firmware/boards" \
    -I"${repo_dir}/firmware/components/katmed_ui/include" \
    -I"${repo_dir}/firmware/components/katmed_core/include" \
    -I"${repo_dir}/firmware/components/katmed_reset/include" \
    -I"${repo_dir}/firmware/components/katmed_images/include" \
    -I"${KATMED_HOST_LVGL_SOURCE:-${repo_dir}/firmware/build-a61-c3/katmed-overrides/lvgl__lvgl}" \
    -c "${repo_dir}/tests/host_c3_alarm_render/check_alarm_panel_transfer.c" \
    -o "${build_dir}/check_alarm_panel_transfer.o"
mapfile -d '' objects < <(find "${build_dir}/CMakeFiles/check_ui_surface.dir" \
    -type f -name '*.o' ! -name 'check_ui_surface.c.o' \
    ! -name 'ui.c.o' -print0)
if [[ -n "${ui_override}" ]]; then
    cc -std=c11 -Wall -Wextra -Werror \
        "-DLV_CONF_PATH=\"${repo_dir}/firmware/components/katmed_ui/host/lv_conf_34k.h\"" \
        -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE \
        -I"${repo_dir}/firmware/components/katmed_ui/include" \
        -I"${repo_dir}/firmware/components/katmed_core/include" \
        -I"${repo_dir}/firmware/components/katmed_reset/include" \
        -I"${repo_dir}/firmware/components/katmed_images/include" \
        -I"${KATMED_HOST_LVGL_SOURCE:-${repo_dir}/firmware/build-a61-c3/katmed-overrides/lvgl__lvgl}" \
        -c "${ui_override}" -o "${build_dir}/ui_override.o"
    objects+=("${build_dir}/ui_override.o")
else
    objects+=("${build_dir}/CMakeFiles/check_ui_surface.dir${repo_dir}/firmware/components/katmed_ui/ui.c.o")
fi
g++ "${build_dir}/check_alarm_panel_transfer.o" "${objects[@]}" \
    "${build_dir}/lvgl/liblvgl.a" -lm \
    -Wl,--wrap=lv_malloc_core -Wl,--wrap=lv_realloc_core \
    -Wl,--wrap=lv_free_core -o "${build_dir}/check_alarm_panel_transfer"
timeout 20s "${build_dir}/check_alarm_panel_transfer"
for panel_case in c3 c3-coalesced c6; do
    timeout 20s "${build_dir}/check_alarm_panel_transfer" "${panel_case}"
done
