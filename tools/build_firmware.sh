#!/usr/bin/env bash
set -euo pipefail
umask 077
repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
board="${1:?Usage: build_firmware.sh c3|c6 [--security-profile] [--private-profile PATH] [idf.py options/actions]}"
shift
private_profile=""
security_profile=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --security-profile) security_profile=1; shift ;;
        --private-profile)
            [[ $# -ge 2 ]] || { echo '--private-profile needs an external config path' >&2; exit 2; }
            [[ -n "$2" ]] || { echo '--private-profile needs an external config path' >&2; exit 2; }
            private_profile="$2"; shift 2 ;;
        *) break ;;
    esac
done
if [[ "$security_profile" -eq 1 && $# -gt 0 ]]; then
    echo 'Security profile accepts build only; no flash, monitor or idf.py actions.' >&2
    exit 2
fi
case "$board" in
    c3) chip=esp32c3 ;;
    c6) chip=esp32c6 ;;
    *) echo "Unsupported board: $board" >&2; exit 2 ;;
esac
if [[ "$security_profile" -eq 1 && -n "$private_profile" && "$board" != c3 ]]; then
    echo 'Private security profile is supported only for c3.' >&2
    exit 2
fi
build_root="${KATMED_BUILD_ROOT:-${repo_dir}/firmware}"
if [[ "$security_profile" -eq 1 && -n "$private_profile" &&
      "${build_root}" != "${repo_dir}/firmware" ]]; then
    # A custom root inside the checkout would bypass firmware/build*/ ignore.
    if ! python3 - "${repo_dir}" "${build_root}" <<'PY'
import pathlib
import sys
repo = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2]).resolve()
raise SystemExit(1 if root.is_relative_to(repo) else 0)
PY
    then
        echo 'Private security build root must be outside the repository.' >&2
        exit 2
    fi
fi
if [[ "${build_root}" != "${repo_dir}/firmware" ]]; then
    mkdir -p -- "${build_root}"
    if [[ -L "${build_root}" || ! -d "${build_root}" || ! -O "${build_root}" ]]; then
        echo "External build root must be a real directory owned by the current user" >&2
        exit 1
    fi
    chmod 700 -- "${build_root}"
fi
profile_suffix=""
profile_label="neutral"
defaults="sdkconfig.defaults;boards/${board}.defaults"
if [[ "$security_profile" -eq 1 ]]; then
    profile_suffix="-security"
    profile_label="security"
    defaults="${defaults};security.defaults;boards/${board}.security.defaults"
    echo 'SECURITY SOURCE BUILD ONLY: do not flash this bootloader; first boot can burn eFuses.' >&2
    if [[ -n "${private_profile}" ]]; then
        profile_suffix="-security-private"
        profile_label="security private"
    fi
fi
build_dir="${build_root}/build-a61-${board}${profile_suffix}"
mkdir -p -- "${build_dir}"
if [[ -L "${build_dir}" || ! -d "${build_dir}" || ! -O "${build_dir}" ]]; then
    echo "Target build directory must be a real directory owned by the current user" >&2
    exit 1
fi
chmod 700 -- "${build_dir}"
if [[ "$security_profile" -eq 1 ]]; then
    # sdkconfig is generated state: defaults do not override an existing file.
    # Regenerate it for every security build, then check the resolved result.
    rm -f -- "${build_dir}/sdkconfig" "${build_dir}/sdkconfig.old"
fi
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
    -D "SDKCONFIG_DEFAULTS=${defaults}" "$@" build \
    >"${build_log}" 2>&1; then
    echo "Firmware build failed; detailed output is retained in the protected build log." >&2
    exit 1
fi

layout_args=()
if [[ "$security_profile" -eq 1 ]]; then layout_args+=(--security-profile); fi
if ! python "${repo_dir}/tools/check_firmware_layout.py" "${board}" \
    --build-dir "${build_dir}" "${layout_args[@]}" >>"${build_log}" 2>&1; then
    echo "Firmware layout check failed; detailed output is retained in the protected build log." >&2
    exit 1
fi
echo "Firmware ${profile_label} build succeeded for ${board}; detailed output is retained in the protected build log."
