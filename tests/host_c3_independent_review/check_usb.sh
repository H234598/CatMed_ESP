#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "${1:-}" in
  fix) source_path="$repo_root/firmware/main/usb_console.c" ;;
  original)
    temp_source="$(mktemp --suffix=.c)"
    trap 'rm -f "$temp_source"' EXIT
    git -C "$repo_root" show d941faee:firmware/main/usb_console.c > "$temp_source"
    source_path="$temp_source"
    ;;
  *) echo 'usage: check_usb.sh fix|original status|busy' >&2; exit 2 ;;
esac
case "${2:-}" in
  status|busy) ;;
  *) echo 'usage: check_usb.sh fix|original status|busy' >&2; exit 2 ;;
esac
binary="$(mktemp)"
trap 'rm -f "$binary" "${temp_source:-}"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
  -I"$repo_root/tests/host_usb" \
  -I"$repo_root/tests/host_c3_independent_review/stubs" \
  -I"$repo_root/firmware/main" \
  -DKATMED_USB_SOURCE="\"$source_path\"" \
  "$repo_root/tests/host_c3_independent_review/usb_framing.c" -o "$binary"
ulimit -c 0
"$binary" "$2"
