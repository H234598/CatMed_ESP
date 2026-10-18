#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp)
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -DKATMED_MESH_HOST \
  -I"$repo/firmware/components/katmed_mesh/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_wire.cpp" \
  "$repo/tests/host_mesh/check_wire.cpp" -l:libcrypto.so.3 -o "$out"
"$out"
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"$repo/firmware/components/katmed_mesh/include" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_session.cpp" \
  "$repo/tests/host_mesh/check_session.cpp" -o "$out"
"$out"
g++ -std=c++17 -Wall -Wextra -Werror -DKATMED_MESH_HOST \
  -I"$repo/firmware/components/katmed_mesh/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  -I"$repo/firmware/components/katmed_store/include" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_payload.cpp" \
  "$repo/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo/tests/host_mesh/check_payload.cpp" -o "$out"
"$out"
