#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-store-XXXXXX)"
cleanup() {
  python3 - "$out" <<'PY'
import pathlib
import shutil
import sys
p = pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path('/tmp') or not p.name.startswith('katmed-store-') or p.is_symlink():
    raise SystemExit('unsafe temporary test directory')
shutil.rmtree(p)
PY
}
trap cleanup EXIT
c++ -std=c++17 -Wall -Wextra -Werror -O1 \
  -I"$repo_root/firmware/components/katmed_core/include" \
  -I"$repo_root/firmware/components/katmed_store/include" \
  "$repo_root/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo_root/firmware/components/katmed_store/katmed_store.cpp" \
  "$repo_root/tests/host_store/check_store.cpp" -lz -o "$out/check_store"
"$out/check_store"
