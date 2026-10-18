#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-store-counters-XXXXXX)"
trap 'python3 - "$out" <<'"'"'PY'"'"'
import pathlib,shutil,sys
p=pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path("/tmp") or not p.name.startswith("katmed-store-counters-") or p.is_symlink():
    raise SystemExit("unsafe temporary directory")
shutil.rmtree(p)
PY' EXIT
g++ -std=c++17 -Wall -Wextra -Werror \
    "-I${repo_root}/firmware/components/katmed_core/include" \
    "-I${repo_root}/firmware/components/katmed_store/include" \
    "${repo_root}/firmware/components/katmed_core/katmed_core.cpp" \
    "${repo_root}/firmware/components/katmed_store/katmed_store.cpp" \
    "${repo_root}/tests/host_longrun_metrics/check_store_counters.cpp" \
    -lz -o "$out/check"
"$out/check"
