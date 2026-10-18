#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
build="${repo}/build-c3-orientation"
mkdir -p "${build}"
cc -std=c11 -Wall -Wextra -Werror \
    -I"${repo}/firmware/boards" \
    -I"${repo}/firmware/components/board/include" \
    -I"${repo}/firmware/components/katmed_core/include" \
    "${repo}/tests/host_c3_display_touch/check_orientation.c" \
    "${repo}/firmware/components/board/board_geometry.c" \
    -o "${build}/check_orientation"
"${build}/check_orientation"
