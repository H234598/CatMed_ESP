#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-mesh-rebind-XXXXXX)"
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$out"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -O1 \
  -I"$repo_root/firmware/components/katmed_core/include" \
  -I"$repo_root/firmware/components/katmed_store/include" \
  "$repo_root/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo_root/firmware/components/katmed_store/katmed_store.cpp" \
  "$repo_root/tests/host_mesh_integration/check_group_rebind.cpp" \
  -lz -o "$out/check_group_rebind"
"$out/check_group_rebind"
