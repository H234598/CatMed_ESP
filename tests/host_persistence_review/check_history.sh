#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-persistence-review-XXXXXX)"
cleanup() {
  python3 - "$out" <<'PY'
import pathlib
import shutil
import sys
p = pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path('/tmp') or not p.name.startswith('katmed-persistence-review-') or p.is_symlink():
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
  "$repo_root/tests/host_persistence_review/check_history.cpp" \
  -lz -o "$out/check_history"
"$out/check_history"
