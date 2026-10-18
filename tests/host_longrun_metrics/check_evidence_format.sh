#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-evidence-format-XXXXXX)"
trap 'rm -rf -- "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    "-I${repo_root}/tests/host_runtime_metrics/stubs" \
    "-I${repo_root}/firmware/main" \
    "${repo_root}/firmware/main/runtime_metrics.c" \
    "${repo_root}/tests/host_longrun_metrics/check_evidence_format.c" \
    -o "$out/check"
"$out/check"
