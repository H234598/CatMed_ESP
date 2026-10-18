#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mktemp -p /tmp katmed-dhcp-transition-XXXXXX)"
trap 'rm -f -- "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -I"$repo/tests/host_dhcp_transition/stubs" \
    -I"$repo/firmware/components/katmed_mesh/include" \
    "$repo/tests/host_dhcp_transition/check.cpp" -o "$out"
"$out"
