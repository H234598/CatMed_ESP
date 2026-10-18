#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp)
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"$repo/firmware/components/katmed_mesh/include" \
  -I"$repo/firmware/components/katmed_store/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_status.cpp" \
  "$repo/tests/host_mesh/check_status.cpp" -lz -o "$out"
"$out"
obj=$(mktemp)
trap 'rm -f "$out" "$obj"' EXIT
cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo/tests/host_mesh/nvs_stubs" \
  -I"$repo/firmware/components/katmed_store/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  -c "$repo/firmware/components/katmed_store/katmed_store_idf.c" -o "$obj"
c++ -std=c++17 -Wall -Wextra -Werror \
  -I"$repo/tests/host_mesh/nvs_stubs" \
  -I"$repo/firmware/components/katmed_store/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  "$repo/tests/host_mesh/check_status_nvs.cpp" -x none "$obj" -o "$out"
"$out"
