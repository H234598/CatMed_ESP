#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
temporary="$(mktemp -d -p /tmp katmed-p7-core-review-XXXXXX)"
trap 'rm -rf "$temporary"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -O1 \
  -I"$repo_root/firmware/components/katmed_core/include" \
  "$repo_root/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo_root/tests/host_p7_review/check_adopted_alarm.cpp" \
  -o "$temporary/check_adopted_alarm"
"$temporary/check_adopted_alarm"
