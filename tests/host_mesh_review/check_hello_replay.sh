#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp)
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -DKATMED_MESH_HOST \
  -I"$repo/tests/host_mesh/stubs" \
  -I"$repo/firmware/components/katmed_mesh/include" \
  -I"$repo/firmware/components/katmed_store/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_wire.cpp" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_payload.cpp" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_session.cpp" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_idf.cpp" \
  "$repo/firmware/components/katmed_store/katmed_store.cpp" \
  "$repo/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo/tests/host_mesh_review/check_hello_replay.cpp" /lib64/libcrypto.so.3 -lz -o "$out"
"$out"
