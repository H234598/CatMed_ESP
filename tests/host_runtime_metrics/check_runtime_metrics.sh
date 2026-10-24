#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-runtime-metrics-XXXXXX)"
cleanup() {
  python3 - "$out" <<'PY'
import pathlib
import shutil
import sys
p = pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path('/tmp') or not p.name.startswith('katmed-runtime-metrics-') or p.is_symlink():
    raise SystemExit('unsafe temporary test directory')
shutil.rmtree(p)
PY
}
trap cleanup EXIT

cc -std=c11 -Wall -Wextra -Werror -pedantic \
   "-I${repo_root}/tests/host_runtime_metrics/stubs" \
   "-I${repo_root}/firmware/main" \
   "-I${repo_root}/firmware/components/board/include" \
   "${repo_root}/firmware/main/runtime_metrics.c" \
   "${repo_root}/tests/host_runtime_metrics/check_runtime_metrics.c" \
   -o "${out}/check_runtime_metrics"
"${out}/check_runtime_metrics"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
   "-DLV_CONF_PATH=\"${repo_root}/firmware/components/katmed_images/host/lv_conf.h\"" \
   -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE -DCONFIG_IDF_TARGET_ESP32C6=1 \
   "-I${repo_root}/tests/host_runtime_metrics/collector_stubs" \
   "-I${repo_root}/firmware/main" \
   "-I${repo_root}/firmware/components/board/include" \
   "-I${repo_root}/firmware/components/katmed_core/include" \
   "-I${repo_root}/firmware/components/katmed_images/include" \
   "-I${repo_root}/firmware/components/katmed_ui/include" \
   "-I${repo_root}/firmware/managed_components/lvgl__lvgl" \
   "${repo_root}/firmware/main/runtime_metrics_idf.c" \
   "${repo_root}/tests/host_runtime_metrics/check_runtime_collect.c" \
   -o "${out}/check_runtime_collect"
"${out}/check_runtime_collect"

# Exact C6 board diagnostic path, physical calls replaced by existing SDK stubs.
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-variable \
   -ffunction-sections -fdata-sections -DCONFIG_IDF_TARGET_ESP32C6=1 \
   "-DLV_CONF_PATH=\"${repo_root}/firmware/components/katmed_images/host/lv_conf.h\"" \
   -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE \
   "-I${repo_root}/tests/host_board_cache_review/stubs" \
   "-I${repo_root}/tests/host_runtime_metrics/collector_stubs" \
   "-I${repo_root}/firmware/components/board/include" \
   "-I${repo_root}/firmware/components/katmed_core/include" \
   "-I${repo_root}/firmware/boards" \
   "-I${repo_root}/firmware/managed_components/lvgl__lvgl" \
   "${repo_root}/tests/host_board_cache_review/check_c6_diagnostics.c" \
   "${repo_root}/firmware/components/board/board_geometry.c" \
   "${repo_root}/firmware/components/board/board_protocol.c" \
   "${repo_root}/firmware/components/board/board_indev_gate.c" \
   "${repo_root}/firmware/components/board/board_orientation.c" \
   -Wl,--gc-sections -o "${out}/check_c6_diagnostics"
"${out}/check_c6_diagnostics"
python3 "${repo_root}/tests/host_runtime_metrics/test_native_diagnostics.py"
