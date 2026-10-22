#!/usr/bin/env bash
set -euo pipefail
repo="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
output="$(mktemp)"
testdir="$(mktemp -d)"
trap 'rm -f "$output"; rm -rf "$testdir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I"$repo/tests/host_security/stubs" \
   -I"$repo/firmware/components/katmed_security/include" \
   "$repo/firmware/components/katmed_security/katmed_nvs_init.c" \
   "$repo/tests/host_security/test_nvs_init.c" -o "$output"
"$output"
"$output" marked-reboot
if [[ -z "${IDF_PATH:-}" ]]; then
    source "$repo/tools/idf_env.sh" >/dev/null 2>&1
fi
python -m esp_idf_nvs_partition_gen generate \
    "$IDF_PATH/components/nvs_flash/nvs_partition_generator/sample_val.csv" \
    "$testdir/plain-nvs.bin" 0x7000 >/dev/null
"$output" plaintext "$testdir/plain-nvs.bin"
"$output" plaintext-marked "$testdir/plain-nvs.bin"

# Exercise the C6 runtime guard against its actual expanded 8 MiB geometry.
cc -std=c11 -Wall -Wextra -Werror -DCONFIG_IDF_TARGET_ESP32C6=1 \
   -I"$repo/tests/host_security/stubs" \
   -I"$repo/firmware/components/katmed_security/include" \
   "$repo/firmware/components/katmed_security/katmed_nvs_init.c" \
   "$repo/tests/host_security/test_nvs_init.c" -o "$output"
"$output"
"$output" marked-reboot
