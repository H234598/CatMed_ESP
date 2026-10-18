#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
lvgl_source="${repo}/firmware/managed_components/lvgl__lvgl"
test -f "$lvgl_source/lvgl.h"
out="$(mktemp -d -p /tmp katmed-km1-phases-XXXXXX)"
cleanup() {
  python3 - "$out" <<'PY'
import pathlib, shutil, sys
p = pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path('/tmp') or not p.name.startswith('katmed-km1-phases-') or p.is_symlink():
    raise SystemExit('unsafe temporary directory')
shutil.rmtree(p)
PY
}
trap cleanup EXIT
flags=(-std=c11 -Wall -Wextra -Werror -Wno-unused-variable
  -ffunction-sections -fdata-sections -DKATMED_FULL_STACK=1 -DKATMED_WITH_CORE=1
  "-DLV_CONF_PATH=\"${repo}/firmware/components/katmed_images/host/lv_conf.h\""
  -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
  "-I${repo}/tests/host_usb" "-I${repo}/tests/host_store/stubs"
  "-I${repo}/tests/host_main/stubs"
  "-I${repo}/firmware/components/board/include"
  "-I${repo}/firmware/components/katmed_ui/include"
  "-I${repo}/firmware/components/katmed_core/include"
  "-I${repo}/firmware/components/katmed_images/include"
  "-I${repo}/firmware/components/katmed_store/include"
  "-I${repo}/firmware/components/katmed_mesh/include"
  "-I${repo}/firmware/components/katmed_time/include"
  "-I${repo}/firmware/main"
  "-I${lvgl_source}")
cc "${flags[@]}" -c "$repo/tests/host_c3_km1_phases/check_product_path.c" -o "$out/main.o"
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
"$out/check" > "$out/product.txt"
python3 "$repo/tests/host_c3_km1_phases/test_phase_host.py" "$out/product.txt"
