#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d /tmp/katmed-names-transactions-XXXXXX)
trap 'rm -rf -- "$out"' EXIT
c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -DKATMED_MESH_HOST \
  -I"$repo/firmware/components/katmed_store/include" -I"$repo/firmware/components/katmed_core/include" \
  "$repo/firmware/components/katmed_names/katmed_names.cpp" "$repo/tests/host_names/check_transactions.cpp" -lz -l:libcrypto.so.3 -o "$out/check"
"$out/check"
