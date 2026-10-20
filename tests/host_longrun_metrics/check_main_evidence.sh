#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-main-evidence-XXXXXX)"
trap 'python3 - "$out" <<'"'"'PY'"'"'
import pathlib,shutil,sys
p=pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path("/tmp") or not p.name.startswith("katmed-main-evidence-") or p.is_symlink():
    raise SystemExit("unsafe temporary directory")
shutil.rmtree(p)
PY' EXIT
flags=(-std=c11 -Wall -Wextra -Werror -Wno-unused-variable
    -ffunction-sections -fdata-sections -DKATMED_FULL_STACK=1 -DKATMED_WITH_CORE=1
    "-DLV_CONF_PATH=\"${repo}/firmware/components/katmed_images/host/lv_conf.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo}/tests/host_store/stubs" "-I${repo}/tests/host_main/stubs"
    "-I${repo}/firmware/components/board/include"
    "-I${repo}/firmware/components/katmed_ui/include"
    "-I${repo}/firmware/components/katmed_core/include"
    "-I${repo}/firmware/components/katmed_images/include"
    "-I${repo}/firmware/components/katmed_store/include"
    "-I${repo}/firmware/components/katmed_config_bridge/include"
    "-I${repo}/firmware/components/katmed_reset/include"
    "-I${repo}/firmware/components/katmed_acme/include"
    "-I${repo}/firmware/components/katmed_web/include"
    "-I${repo}/firmware/components/katmed_mesh/include"
    "-I${repo}/firmware/components/katmed_time/include"
    "-I${repo}/firmware/main")
CPATH="${repo}/firmware/managed_components/lvgl__lvgl" \
    cc "${flags[@]}" -c "$repo/tests/host_longrun_metrics/check_main_evidence.c" -o "$out/main.o"
cc "${flags[@]}" -c "$repo/firmware/components/katmed_ui/navigation.c" -o "$out/navigation.o"
cc "${flags[@]}" -c "$repo/firmware/components/katmed_time/katmed_time.c" -o "$out/time.o"
c++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo/firmware/components/katmed_core/include" \
    -I"$repo/firmware/components/katmed_store/include" \
    -c "$repo/firmware/components/katmed_core/katmed_core.cpp" -o "$out/core.o"
c++ -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo/firmware/components/katmed_core/include" \
    -I"$repo/firmware/components/katmed_store/include" \
    -c "$repo/firmware/components/katmed_store/katmed_store.cpp" -o "$out/store.o"
c++ -Wl,--gc-sections "$out/main.o" "$out/navigation.o" "$out/time.o" \
    "$out/core.o" "$out/store.o" -lz -o "$out/check"
"$out/check"
