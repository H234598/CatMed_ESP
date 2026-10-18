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
   "${repo_root}/firmware/main/runtime_metrics.c" \
   "${repo_root}/tests/host_runtime_metrics/check_runtime_metrics.c" \
   -o "${out}/check_runtime_metrics"
"${out}/check_runtime_metrics"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
   "-DLV_CONF_PATH=\"${repo_root}/firmware/components/katmed_images/host/lv_conf.h\"" \
   -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE \
   "-I${repo_root}/tests/host_runtime_metrics/collector_stubs" \
   "-I${repo_root}/firmware/main" \
   "-I${repo_root}/firmware/components/katmed_core/include" \
   "-I${repo_root}/firmware/components/katmed_images/include" \
   "-I${repo_root}/firmware/components/katmed_ui/include" \
   "-I${repo_root}/firmware/managed_components/lvgl__lvgl" \
   "${repo_root}/firmware/main/runtime_metrics_idf.c" \
   "${repo_root}/tests/host_runtime_metrics/check_runtime_collect.c" \
   -o "${out}/check_runtime_collect"
"${out}/check_runtime_collect"
