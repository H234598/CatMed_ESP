#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-main-runtime-XXXXXX)"
cleanup() {
  python3 - "$out" <<'PY'
import pathlib
import shutil
import sys
p = pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path('/tmp') or not p.name.startswith('katmed-main-runtime-') or p.is_symlink():
    raise SystemExit('unsafe temporary test directory')
shutil.rmtree(p)
PY
}
trap cleanup EXIT
flags=(-std=c11 -Wall -Wextra -Werror -Wno-unused-variable
    -ffunction-sections -fdata-sections
    -DKATMED_FULL_STACK=1 -DKATMED_WITH_CORE=1
    "-DLV_CONF_PATH=\"${repo_root}/firmware/components/katmed_images/host/lv_conf.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo_root}/tests/host_store/stubs"
    "-I${repo_root}/tests/host_main/stubs"
    "-I${repo_root}/firmware/components/board/include"
    "-I${repo_root}/firmware/components/katmed_ui/include"
    "-I${repo_root}/firmware/components/katmed_core/include"
    "-I${repo_root}/firmware/components/katmed_images/include"
    "-I${repo_root}/firmware/components/katmed_store/include"
    "-I${repo_root}/firmware/main"
    "-I${repo_root}/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "${repo_root}/tests/host_runtime_metrics/check_main_runtime.c" -o "${out}/main.o"
cc "${flags[@]}" -c "${repo_root}/firmware/main/runtime_metrics.c" -o "${out}/metrics.o"
c++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    "-I${repo_root}/firmware/components/katmed_core/include" \
    "-I${repo_root}/firmware/components/katmed_store/include" \
    -c "${repo_root}/firmware/components/katmed_core/katmed_core.cpp" -o "${out}/core.o"
c++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    "-I${repo_root}/firmware/components/katmed_core/include" \
    "-I${repo_root}/firmware/components/katmed_store/include" \
    -c "${repo_root}/firmware/components/katmed_store/katmed_store.cpp" -o "${out}/store.o"
c++ -Wl,--gc-sections -Wl,--wrap=katmed_runtime_metrics_collect \
    "${out}/main.o" "${out}/metrics.o" "${out}/core.o" "${out}/store.o" -lz \
    -o "${out}/check_main_runtime"
timeout 20s "${out}/check_main_runtime"
