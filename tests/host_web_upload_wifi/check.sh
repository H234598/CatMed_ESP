#!/usr/bin/env bash
set -eu
python3 "$(dirname "$0")/check_contract.py"
test_dir="$(cd "$(dirname "$0")" && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -I"$test_dir/stubs" -I"$test_dir/../../firmware/components/katmed_web/include" \
  -I"$test_dir/../../firmware/components/katmed_web" \
  "$test_dir/check_wifi_callbacks.c" \
  "$test_dir/../../firmware/components/katmed_web/katmed_web_wifi.c" \
  -Wl,--gc-sections -o "$tmp_dir/check_wifi_callbacks"
"$tmp_dir/check_wifi_callbacks"
gcc -std=c11 -Wall -Wextra -Werror \
  -I"$test_dir/stubs" \
  -I"$test_dir/../../firmware/components/katmed_web/include" \
  -I"$test_dir/../../firmware/components/katmed_web" \
  "$test_dir/check_media_routes.c" \
  "$test_dir/../../firmware/components/katmed_web/katmed_web_media.c" \
  -o "$tmp_dir/check_media_routes"
"$tmp_dir/check_media_routes"

gcc -std=c11 -Wall -Wextra -Werror -DKATMED_WEB_WIFI_TEST_WIPE_OBSERVER \
  -I"$test_dir/stubs" -I"$test_dir/../../firmware/components/katmed_web/include" \
  -I"$test_dir/../../firmware/components/katmed_web" \
  "$test_dir/check_wifi_reset_routes.c" "$test_dir/stubs/cJSON.c" \
  "$test_dir/../../firmware/components/katmed_web/katmed_web_wifi.c" \
  "$test_dir/../../firmware/components/katmed_web/katmed_web_reset.c" \
  -o "$tmp_dir/check_wifi_reset_routes"
"$tmp_dir/check_wifi_reset_routes"
