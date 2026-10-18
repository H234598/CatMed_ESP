#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$repo_root/firmware/components/board/include" \
  -I"$repo_root/firmware/components/katmed_core/include" \
  "$repo_root/tests/host_c3_independent_review/decoder.c" \
  "$repo_root/firmware/components/board/board_protocol.c" \
  "$repo_root/firmware/components/board/board_geometry.c" \
  "$repo_root/firmware/components/board/board_indev_gate.c" -o "$binary"
"$binary"
