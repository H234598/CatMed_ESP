#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "${1:-}" in
  fix) source_path="$repo_root/firmware/components/board/board.c" ;;
  original)
    temp_source="$(mktemp --suffix=.c)"
    git -C "$repo_root" show d941faee:firmware/components/board/board.c > "$temp_source"
    source_path="$temp_source"
    ;;
  *) echo 'usage: check_color.sh fix|original 3|6' >&2; exit 2 ;;
esac
case "${2:-}" in 3|6) ;; *) echo 'usage: check_color.sh fix|original 3|6' >&2; exit 2 ;; esac
binary="$(mktemp)"
trap 'rm -f "$binary" "${temp_source:-}"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-variable \
  -ffunction-sections -fdata-sections -DCONFIG_IDF_TARGET_ESP32C"$2"=1 \
  -I"$repo_root/tests/host_board_cache_review/stubs" \
  -I"$repo_root/firmware/boards" \
  -I"$repo_root/firmware/components/board/include" \
  -I"$repo_root/firmware/components/katmed_core/include" \
  -I"$repo_root/firmware/managed_components/lvgl__lvgl" \
  -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE \
  -DLV_CONF_PATH="\"$repo_root/firmware/components/katmed_images/host/lv_conf.h\"" \
  -DKATMED_BOARD_SOURCE="\"$source_path\"" \
  "$repo_root/tests/host_c3_independent_review/board_color.c" \
  -Wl,--gc-sections -o "$binary"
ulimit -c 0
"$binary"
