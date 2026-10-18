#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp)
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -ftrapv \
 -I"$repo/firmware/components/katmed_mesh/include" -I"$repo/firmware/components/katmed_core/include" -I"$repo/firmware/components/katmed_store/include" \
 "$repo/firmware/components/katmed_mesh/katmed_mesh_status.cpp" "$repo/tests/host_mesh/check_admission.cpp" -lz -o "$out"
"$out"
