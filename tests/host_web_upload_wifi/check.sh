#!/usr/bin/env bash
set -eu
python3 "$(dirname "$0")/check_contract.py"
test_dir="$(cd "$(dirname "$0")" && pwd)"
store_include="$test_dir/../../firmware/components/katmed_images/include"
if [ ! -f "$store_include/katmed_image_store.h" ]; then
  store_include="$test_dir/../../firmware/components/katmed_images/include"
fi
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -I"$test_dir/stubs" -I"$(dirname "$test_dir")/../firmware/components/katmed_web" \
  "$test_dir/check_wifi_callbacks.c" \
  "$(dirname "$test_dir")/../firmware/components/katmed_web/katmed_web_wifi.c" \
  -Wl,--gc-sections -o "$tmp_dir/check_wifi_callbacks"
"$tmp_dir/check_wifi_callbacks"
gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only \
  -I"$test_dir/stubs" \
  -I"$store_include" \
  -I"$test_dir/../../firmware/components/katmed_web" \
  "$test_dir/../../firmware/components/katmed_web/katmed_web_media.c"
gcc -std=c11 -Wall -Wextra -Werror \
  -I"$test_dir/stubs" -I"$store_include" \
  -I"$test_dir/../../firmware/components/katmed_web" \
  "$test_dir/check_media_routes.c" \
  "$test_dir/../../firmware/components/katmed_web/katmed_web_media.c" \
  -o "$tmp_dir/check_media_routes"
"$tmp_dir/check_media_routes"
