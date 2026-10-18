#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -I"$repo_root/firmware/components/katmed_core/include" \
  "$repo_root/tests/host_c3_independent_review/alarm_time.cpp" \
  "$repo_root/firmware/components/katmed_core/katmed_core.cpp" -o "$binary"
"$binary"
