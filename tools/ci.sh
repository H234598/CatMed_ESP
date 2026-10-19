#!/usr/bin/env bash
# Public CI uses the neutral standard profile; no private profile is needed.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"
mode=${1:?Usage: tools/ci.sh dependencies|host|firmware [c3|c6]}
board=${2:-c3}
case "$board" in c3) target=esp32c3;; c6) target=esp32c6;; *) exit 2;; esac
if [[ "$mode" == dependencies || "$mode" == firmware ]]; then
    : "${IDF_PATH:?Use the ESP-IDF 6.1 environment}"
    # The SDK lives in the disposable CI container, never a developer's SDK.
    if [[ ${CI:-} != true ]]; then
        echo 'Synthetic firmware setup is restricted to disposable CI.' >&2
        exit 2
    fi
    python3 tests/check_idf6_lwip_compat.py
    python tools/private_wifi_defaults.py \
        --output "$repo/firmware/build-$board/generated/private_wifi_defaults.h" \
        --ui-profile-output "$repo/firmware/build-$board/generated/katmed_private_ui_profile.h"
    action=reconfigure
    [[ "$mode" != firmware ]] || action=build
    (cd firmware && idf.py -B "build-$board" -D "IDF_TARGET=$target" \
        -D "SDKCONFIG=$repo/firmware/build-$board/sdkconfig" \
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/$board.defaults" "$action")
    if [[ "$mode" == firmware ]]; then
        python tools/check_firmware_layout.py "$board"
    fi
elif [[ "$mode" == host ]]; then
    python3 tests/check_crop_geometry.py
    cmake -S . -B build-ci-host -G Ninja
    cmake --build build-ci-host -j 4
    ctest --test-dir build-ci-host --output-on-failure
    cmake -S firmware/components/katmed_ui/host -B build-ci-ui -G Ninja \
        -DKATMED_UI_HOST_MEM34=ON
    cmake --build build-ci-ui -j 4
    ctest --test-dir build-ci-ui --output-on-failure
    mkdir -p build-ci-ui/surface
    build-ci-ui/check_ui_surface 240 240 0 build-ci-ui/surface c3
    build-ci-ui/check_ui_surface 172 320 0 build-ci-ui/surface c6
    build-ci-ui/check_ui_surface 320 172 1 build-ci-ui/surface c6-landscape
    # Existing narrow main-contact check expects bootstrap tools at this path.
    export KATMED_TOOLS_DIR="$repo/build-ci-tools"
    mkdir -p "$KATMED_TOOLS_DIR/bootstrap/bin"
    ln -sf "$(command -v cmake)" "$KATMED_TOOLS_DIR/bootstrap/bin/cmake"
    ln -sf "$(command -v ninja)" "$KATMED_TOOLS_DIR/bootstrap/bin/ninja"
    bash tests/host_main/check_main_contact.sh
    bash tests/host_mesh/check_interaction.sh
    bash tests/host_mesh/check_admission.sh
    bash tests/host_store/check_store.sh
    bash tests/host_config_bridge/check_bridge.sh
    bash tests/host_web_auth/check_web_auth.sh
    node tests/host_web_ui/check_async_buttons.js
    python3 tests/host_web_ui/check_contract.py
    bash tests/host_store/check_main_submit.sh
    bash tests/host_mesh/check_engine.sh
    bash tests/host_mesh/check_status.sh
    bash tests/host_mesh/check_wire.sh
else
    echo 'Unknown CI action' >&2
    exit 2
fi
