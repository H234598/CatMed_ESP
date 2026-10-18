#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp)
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -ftrapv \
 -I"$repo/firmware/components/katmed_mesh/include" -I"$repo/firmware/components/katmed_core/include" \
 "$repo/tests/host_mesh/check_interaction.cpp" -o "$out"
"$out"
