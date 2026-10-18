#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-core-boot-XXXXXX)"
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$out"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -O1 \
  -I"$repo_root/firmware/components/katmed_core/include" \
  "$repo_root/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo_root/tests/host_mesh_integration/check_core_boot_adopt.cpp" \
  -o "$out/check_core_boot_adopt"
"$out/check_core_boot_adopt"
