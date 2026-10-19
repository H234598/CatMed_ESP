#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d "${TMPDIR:-/tmp}/katmed-store-XXXXXX")"
cleanup() {
  python3 - "$out" <<'PY'
import os
import pathlib
import shutil
import sys
p = pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path(os.environ.get('TMPDIR', '/tmp')).resolve() or not p.name.startswith('katmed-store-') or p.is_symlink():
    raise SystemExit('unsafe temporary test directory')
shutil.rmtree(p)
PY
}
trap cleanup EXIT
flags=(-std=c11 -Wall -Wextra -Werror -Wno-error=cpp -Wno-unused-variable -ffunction-sections -fdata-sections
    -DKATMED_FULL_STACK=1 -DKATMED_WITH_CORE=1
    "-DLV_CONF_PATH=\"${repo_root}/firmware/components/katmed_images/host/lv_conf.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo_root}/tests/host_store/stubs"
    "-I${repo_root}/tests/host_main/stubs"
    "-I${repo_root}/firmware/components/board/include"
    "-I${repo_root}/firmware/components/katmed_ui/include"
    "-I${repo_root}/firmware/components/katmed_core/include"
    "-I${repo_root}/firmware/components/katmed_config_bridge/include"
    "-I${repo_root}/firmware/components/katmed_reset/include"
    "-I${repo_root}/firmware/components/katmed_acme/include"
    "-I${repo_root}/firmware/components/katmed_web/include"
    "-I${repo_root}/firmware/components/katmed_images/include"
    "-I${repo_root}/firmware/components/katmed_store/include"
    "-I${repo_root}/firmware/components/katmed_mesh/include"
    "-I${repo_root}/firmware/components/katmed_time/include"
    "-I${repo_root}/firmware/main"
    "-I${repo_root}/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "$repo_root/tests/host_store/check_main_submit.c" -o "$out/main.o"
cc "${flags[@]}" -c "$repo_root/firmware/components/katmed_ui/navigation.c" -o "$out/navigation.o"
cc "${flags[@]}" -c "$repo_root/firmware/components/katmed_time/katmed_time.c" -o "$out/time.o"
c++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo_root/firmware/components/katmed_core/include" \
    -I"$repo_root/firmware/components/katmed_store/include" \
    -c "$repo_root/firmware/components/katmed_core/katmed_core.cpp" -o "$out/core.o"
c++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo_root/firmware/components/katmed_core/include" \
    -I"$repo_root/firmware/components/katmed_store/include" \
    -c "$repo_root/firmware/components/katmed_store/katmed_store.cpp" -o "$out/store.o"
c++ -Wl,--gc-sections "$out/main.o" "$out/navigation.o" "$out/time.o" "$out/core.o" "$out/store.o" -lz -o "$out/check_main_submit"
"$out/check_main_submit"
