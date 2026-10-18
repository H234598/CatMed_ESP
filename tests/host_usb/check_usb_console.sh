#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
  -I"$repo_root/tests/host_usb" -I"$repo_root/firmware/main" \
  "$repo_root/tests/host_usb/test_usb_console.c" -o "$binary"
"$binary"
