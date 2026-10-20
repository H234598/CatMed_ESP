#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${repo_dir}/firmware/build-host-reset-physical-ui"
tools_dir="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
export PATH="$tools_dir/bootstrap/bin:$PATH"
"$tools_dir/bootstrap/bin/cmake" -S "${repo_dir}/tests/host_reset_physical/ui" -B "$build_dir" -G Ninja >/dev/null
"$tools_dir/bootstrap/bin/cmake" --build "$build_dir" --target check_reset_ui -j 4 >/dev/null
mkdir -p "$build_dir/surface"
"$build_dir/check_reset_ui" 172 320 0 "$build_dir/surface" c6 >/dev/null
echo 'physical reset LVGL host flow: OK'
