#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fixture="$(mktemp -d)"
trap 'rm -rf "$fixture"' EXIT
ulimit -c 0
mkdir -p "$fixture/firmware/main" "$fixture/tests/host_usb" \
  "$fixture/tests/host_c3_usb_framing"
cp -a "$repo_root/tests/host_usb/freertos" "$fixture/tests/host_usb/"
cp -a "$repo_root/tests/host_c3_usb_framing/." \
  "$fixture/tests/host_c3_usb_framing/"
cp "$repo_root/firmware/main/usb_console.h" \
   "$repo_root/firmware/main/runtime_metrics.c" \
   "$repo_root/firmware/main/runtime_metrics.h" "$fixture/firmware/main/"
git -C "$repo_root" show d941faee3e6f6887611bb17e5fde87d237705ef6:firmware/main/usb_console.c \
  > "$fixture/firmware/main/usb_console.c"
set +e
bash "$fixture/tests/host_c3_usb_framing/check.sh"
result=$?
set -e
printf 'Originalfassung: Repro-Exitcode %s (erwartet 134)\n' "$result"
test "$result" -eq 134
