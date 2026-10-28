#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
out="${repo}/build-ui-host/c3-repro"
mkdir -p "$out"
for target in 3 6; do
    gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-variable \
        -ffunction-sections -fdata-sections -DCONFIG_IDF_TARGET_ESP32C${target}=1 \
        -I"${repo}/tests/host_board_cache_review/stubs" \
        -I"${repo}/firmware/boards" \
        -I"${repo}/firmware/components/board/include" \
        -I"${repo}/firmware/components/katmed_core/include" \
        -I"${repo}/firmware/managed_components/lvgl__lvgl" \
        -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE \
        "-DLV_CONF_PATH=\"${repo}/firmware/components/katmed_images/host/lv_conf.h\"" \
        "${repo}/tests/host_c3_display_touch/color_profile.c" -Wl,--gc-sections \
        -o "${out}/color_profile_c${target}"
    "${out}/color_profile_c${target}"
done
