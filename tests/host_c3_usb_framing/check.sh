#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
  -I"$repo_root/tests/host_usb" \
  -I"$repo_root/tests/host_c3_usb_framing/stubs" \
  -I"$repo_root/firmware/main" \
  "$repo_root/tests/host_c3_usb_framing/test_usb_framing.c" -o "$binary"
"$binary"
