#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
out="$(mktemp -d /tmp/katmed-orientation-XXXXXX)"
trap 'rm -f "$out/check_orientation"; rmdir "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo/firmware/components/board/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  "$repo/firmware/components/board/board_orientation.c" \
  "$repo/firmware/components/board/board_geometry.c" \
  "$repo/firmware/components/board/board_indev_gate.c" \
  "$repo/tests/host_orientation/check_orientation.c" -o "$out/check_orientation"
"$out/check_orientation"
