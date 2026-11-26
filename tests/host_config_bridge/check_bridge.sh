#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-config-bridge-XXXXXX)"
trap 'rm -rf -- "$out"' EXIT
cc -std=c11 -D_POSIX_C_SOURCE=200809L -DKATMED_CONFIG_BRIDGE_TEST_TICK \
  -Wall -Wextra -Werror -pthread \
  -I"$repo_root/tests/host_config_bridge/stubs" \
  -I"$repo_root/firmware/components/katmed_config_bridge/include" \
  -I"$repo_root/firmware/components/katmed_core/include" \
  "$repo_root/firmware/components/katmed_config_bridge/katmed_config_bridge.c" \
  "$repo_root/tests/host_config_bridge/check_bridge.c" -o "$out/check_bridge"
"$out/check_bridge"
