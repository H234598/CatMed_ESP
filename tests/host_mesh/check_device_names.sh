#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d -p /tmp katmed-device-names-XXXXXX)
cleanup() {
    python3 - "$out" <<'PY'
import pathlib
import shutil
import sys
p = pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path('/tmp') or not p.name.startswith('katmed-device-names-') or p.is_symlink():
    raise SystemExit('unsafe temporary test directory')
shutil.rmtree(p)
PY
}
trap cleanup EXIT
c++ -std=c++17 -Wall -Wextra -Werror -O1 \
  -I"$repo/firmware/components/katmed_mesh/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  -I"$repo/firmware/components/katmed_store/include" \
  "$repo/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo/firmware/components/katmed_store/katmed_store.cpp" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_payload.cpp" \
  "$repo/tests/host_mesh/check_device_names.cpp" -lz -o "$out/check_device_names"
"$out/check_device_names"
