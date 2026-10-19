#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
board="${1:?Usage: build_firmware.sh c3|c6 [--private-profile PATH] [idf.py options/actions]}"
shift
private_profile=""
if [[ "${1:-}" == "--private-profile" ]]; then
    [[ $# -ge 2 ]] || { echo '--private-profile needs an external config path' >&2; exit 2; }
    private_profile="$2"
    shift 2
fi
case "$board" in
    c3) chip=esp32c3 ;;
    c6) chip=esp32c6 ;;
    *) echo "Unsupported board: $board" >&2; exit 2 ;;
esac
build_root="${KATMED_BUILD_ROOT:-${repo_dir}/firmware}"
if [[ "${build_root}" != "${repo_dir}/firmware" ]]; then
    mkdir -p -- "${build_root}"
    if [[ -L "${build_root}" || ! -d "${build_root}" || ! -O "${build_root}" ]]; then
        echo "External build root must be a real directory owned by the current user" >&2
        exit 1
    fi
    chmod 700 -- "${build_root}"
fi
build_dir="${build_root}/build-a61-${board}"
mkdir -p -- "${build_dir}"
if [[ -L "${build_dir}" || ! -d "${build_dir}" || ! -O "${build_dir}" ]]; then
    echo "Target build directory must be a real directory owned by the current user" >&2
    exit 1
fi
chmod 700 -- "${build_dir}"
if [[ -n "${private_profile}" ]]; then
    python3 "${repo_dir}/tools/private_wifi_defaults.py" \
        --config "${private_profile}" \
        --output "${build_dir}/generated/private_wifi_defaults.h" \
        --ui-profile-output "${build_dir}/generated/katmed_private_ui_profile.h"
else
    python3 "${repo_dir}/tools/private_wifi_defaults.py" \
        --output "${build_dir}/generated/private_wifi_defaults.h" \
        --ui-profile-output "${build_dir}/generated/katmed_private_ui_profile.h"
fi
python3 "${repo_dir}/tools/prepare_images.py" --verify-only > /dev/null
source "${repo_dir}/tools/idf_env.sh"
python3 "${repo_dir}/tools/prepare_iot_bridge_compat.py" \
    --component "${repo_dir}/firmware/managed_components/espressif__iot_bridge" --idf "${IDF_PATH}"
cd "${repo_dir}/firmware"
# SDK config, CMake state and component lock are target-local. Never reuse a
# C3 sdkconfig or dependency solution for C6 (or the other way around).
export IDF_PY_BUILD_JOBS="${IDF_PY_BUILD_JOBS:-4}"
build_log="$(mktemp "${build_dir}/private-build.XXXXXX.log")"
if ! idf.py -B "${build_dir}" -D "IDF_TARGET=${chip}" \
    -D "SDKCONFIG=${build_dir}/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/${board}.defaults" "$@" build \
    >"${build_log}" 2>&1; then
    echo "Firmware build failed; detailed output is retained in the protected build log." >&2
    exit 1
fi

if ! python "${repo_dir}/tools/check_firmware_layout.py" "${board}" \
    --build-dir "${build_dir}" >>"${build_log}" 2>&1; then
    echo "Firmware layout check failed; detailed output is retained in the protected build log." >&2
    exit 1
fi
echo "Firmware build succeeded for ${board}; detailed output is retained in the protected build log."
