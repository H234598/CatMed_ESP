#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
out_dir="${repo_dir}/build-c3-alarm-time"
mkdir -p "${out_dir}"
g++ -std=c++17 -Wall -Wextra -Werror -O2 \
    -I"${repo_dir}/firmware/components/katmed_core/include" \
    "${repo_dir}/tests/host_c3_alarm_time/check_alarm_time.cpp" \
    "${repo_dir}/firmware/components/katmed_core/katmed_core.cpp" \
    -o "${out_dir}/check_alarm_time"
"${out_dir}/check_alarm_time"
