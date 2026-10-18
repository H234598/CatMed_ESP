#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
out_dir="${repo_dir}/build-c3-display-budget"
mkdir -p "${out_dir}"
cc -std=c11 -Wall -Wextra -Werror \
    -I"${repo_dir}/firmware/components/board/include" \
    "${repo_dir}/tests/host_c3_display_budget/check_display_budget.c" \
    -o "${out_dir}/check_display_budget"
"${out_dir}/check_display_budget"
