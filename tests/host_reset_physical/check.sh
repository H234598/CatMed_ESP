#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
gcc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_dir/tests/host_reset_physical/stubs" \
  -I"$repo_dir/tests/host_web_upload_wifi/stubs" \
  -I"$repo_dir/firmware/components/katmed_reset/include" \
  "$repo_dir/tests/host_reset_physical/check.c" \
  "$repo_dir/firmware/components/katmed_reset/katmed_reset_idf.c" \
  "$repo_dir/firmware/components/katmed_reset/katmed_reset_journal.c" \
  -o "$tmp_dir/check"
"$tmp_dir/check"
