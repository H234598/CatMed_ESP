#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
: "${KATMED_BOARD_CACHE_BUILD_DIR:?Set the existing board-cache build directory}"
: "${KATMED_TEST_BUILD_DIR:?Set a writable test build directory}"
mkdir -p -- "$KATMED_TEST_BUILD_DIR"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-unused-variable \
  -ffunction-sections -fdata-sections -DCONFIG_IDF_TARGET_ESP32C3=1 \
  "-DLV_CONF_PATH=\"$repo/firmware/components/katmed_images/host/lv_conf.h\"" \
  -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE \
  "-I$repo/tests/host_board_cache_review/stubs" "-I$repo/firmware/boards" \
  "-I$repo/firmware/components/board/include" "-I$repo/firmware/components/katmed_core/include" \
  "-I${KATMED_HOST_LVGL_SOURCE:-$repo/firmware/build-a61-c3/katmed-overrides/lvgl__lvgl}" \
  "$repo/tests/host_board_cache_review/check_c3_touch_health.c" \
  "$repo/firmware/components/board/board_touch_buffer.c" \
  "$repo/firmware/components/board/board_geometry.c" \
  "$repo/firmware/components/board/board_protocol.c" \
  "$repo/firmware/components/board/board_indev_gate.c" \
  "$KATMED_BOARD_CACHE_BUILD_DIR/lvgl/liblvgl.a" -Wl,--gc-sections -lm \
  -o "$KATMED_TEST_BUILD_DIR/c3-health-test"
"$KATMED_TEST_BUILD_DIR/c3-health-test"
