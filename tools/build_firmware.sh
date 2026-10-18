#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
board="${1:?Usage: build_firmware.sh c3|c6 [idf.py options/actions]}"
shift
case "$board" in
    c3) chip=esp32c3 ;;
    c6) chip=esp32c6 ;;
    *) echo "Unsupported board: $board" >&2; exit 2 ;;
esac
build_dir="${repo_dir}/firmware/build-${board}"
mkdir -p -- "${build_dir}"
if [[ -L "${build_dir}" || ! -d "${build_dir}" || ! -O "${build_dir}" ]]; then
    echo "Target build directory must be a real directory owned by the current user" >&2
    exit 1
fi
chmod 700 -- "${build_dir}"
python3 "${repo_dir}/tools/private_wifi_defaults.py" \
    --config "${HOME}/.config/katmed_esp/private_wifi.json" \
    --output "${build_dir}/generated/private_wifi_defaults.h"
python3 "${repo_dir}/tools/prepare_images.py" --verify-only > /dev/null
source "${repo_dir}/tools/idf_env.sh"
python "${repo_dir}/tools/prepare_idf_compat.py" "${IDF_PATH}" --apply
cd "${repo_dir}/firmware"
# SDK config, CMake state and component lock are target-local. Never reuse a
# C3 sdkconfig or dependency solution for C6 (or the other way around).
export IDF_PY_BUILD_JOBS="${IDF_PY_BUILD_JOBS:-4}"
build_log="$(mktemp "${build_dir}/private-build.XXXXXX.log")"
if ! idf.py -B "build-${board}" -D "IDF_TARGET=${chip}" \
    -D "SDKCONFIG=${repo_dir}/firmware/build-${board}/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/${board}.defaults" "$@" build \
    >"${build_log}" 2>&1; then
    echo "Firmware build failed; detailed output is retained in the protected build log." >&2
    exit 1
fi

if ! python "${repo_dir}/tools/check_firmware_layout.py" "${board}" \
    >>"${build_log}" 2>&1; then
    echo "Firmware layout check failed; detailed output is retained in the protected build log." >&2
    exit 1
fi
echo "Firmware build succeeded for ${board}; detailed output is retained in the protected build log."
