#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror \
  -I"${repo_dir}/firmware/components/katmed_web/include" \
  "${repo_dir}/firmware/components/katmed_web/katmed_web_auth.c" \
  "${repo_dir}/tests/host_web_auth/check_web_auth.c" \
  -o "$tmp_dir/web_auth"
"$tmp_dir/web_auth"
cc -std=gnu11 -Wall -Wextra -Werror \
  -I"${repo_dir}/firmware/components/katmed_web/include" \
  "${repo_dir}/firmware/components/katmed_web/katmed_web_policy.c" \
  "${repo_dir}/tests/host_web_auth/check_web_policy.c" \
  -o "$tmp_dir/web_policy"
"$tmp_dir/web_policy"
