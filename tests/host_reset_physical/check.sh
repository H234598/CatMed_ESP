#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
gcc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_dir/tests/host_reset_physical/stubs" \
  -I"$repo_dir/tests/host_web_upload_wifi/stubs" \
  -I"$repo_dir/firmware/components/katmed_reset/include" \
  -I"$repo_dir/firmware/components/katmed_names/include" \
  -I"$repo_dir/firmware/components/katmed_security/include" \
  "$repo_dir/tests/host_reset_physical/check.c" \
  "$repo_dir/firmware/components/katmed_reset/katmed_reset_idf.c" \
  "$repo_dir/firmware/components/katmed_reset/katmed_reset_journal.c" \
  -o "$tmp_dir/check"
"$tmp_dir/check"
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -I"$repo_dir/firmware/components/katmed_reset" \
  "$repo_dir/tests/host_reset_physical/check_acme_files.c" \
  "$repo_dir/firmware/components/katmed_reset/katmed_reset_acme_files.c" \
  -o "$tmp_dir/check_acme_files"
"$tmp_dir/check_acme_files"
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -I"$repo_dir/firmware/boards" \
  -I"$repo_dir/firmware/components/katmed_reset/include" \
  "$repo_dir/tests/check_reset_workflow.c" \
  "$repo_dir/firmware/components/katmed_reset/katmed_reset_journal.c" \
  -o "$tmp_dir/check_reset_workflow"
"$tmp_dir/check_reset_workflow"
gcc -std=c11 -DKATMED_APPSTORE_MAINTENANCE=1 -Wall -Wextra -Werror \
  -I"$repo_dir/tests/host_reset_physical/stubs" \
  -I"$repo_dir/tests/host_web_upload_wifi/stubs" \
  -I"$repo_dir/firmware/components/katmed_reset/include" \
  -I"$repo_dir/firmware/components/katmed_names/include" \
  -I"$repo_dir/firmware/components/katmed_security/include" \
  "$repo_dir/tests/host_reset_physical/check_appmaint.c" \
  "$repo_dir/firmware/components/katmed_reset/katmed_reset_idf.c" \
  "$repo_dir/firmware/components/katmed_reset/katmed_reset_journal.c" \
  -Wl,--wrap=opendir -Wl,--wrap=readdir -Wl,--wrap=closedir \
  -Wl,--wrap=stat -Wl,--wrap=unlink -Wl,--wrap=rmdir \
  -o "$tmp_dir/check_appmaint"
"$tmp_dir/check_appmaint"
gcc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_dir/tests/host_reset_physical/stubs" \
  -I"$repo_dir/firmware/main" \
  "$repo_dir/tests/host_reset_physical/check_appmaint_target.c" \
  -o "$tmp_dir/check_appmaint_target"
"$tmp_dir/check_appmaint_target"
