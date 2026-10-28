#!/usr/bin/env bash
# Public CI uses the neutral standard profile; no private profile is needed.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"
mode=${1:?Usage: tools/ci.sh dependencies|host|firmware|qlty [c3|c6]}
board=${2:-c3}
case "$board" in c3) target=esp32c3;; c6) target=esp32c6;; *) exit 2;; esac
if [[ "$mode" == qlty ]]; then
    umask 077
    qlty_work=$(mktemp -d "${TMPDIR:-/tmp}/katmed-qlty.XXXXXXXX")
    trap 'rm -rf -- "$qlty_work"' EXIT
    qlty_bin=${QLTY_BIN:-}
    if [[ -z "$qlty_bin" ]]; then
        curl --fail --silent --show-error --location \
            https://github.com/qltysh/qlty/releases/download/v0.662.0/qlty-x86_64-unknown-linux-musl.tar.xz \
            --output "$qlty_work/qlty.tar.xz"
        printf '%s  %s\n' 49eb3539abe3d78f77a57f0e78b0d638cd76d880ddd807be9a1a7bad396fd1a7 \
            "$qlty_work/qlty.tar.xz" | sha256sum --check --status
        tar -xJf "$qlty_work/qlty.tar.xz" -C "$qlty_work"
        qlty_bin="$qlty_work/qlty-x86_64-unknown-linux-musl/qlty"
    fi
    [[ $("$qlty_bin" --version) == 'qlty 0.662.0 '* ]]
    QLTY_BIN="$qlty_bin" python3 tests/host_tooling_qlty58/test_qlty_gate.py
    : "${QLTY_BASE_REF:?Exact public baseline commit required}"
    mkdir "$qlty_work/baseline" "$qlty_work/candidate"
    git archive "$QLTY_BASE_REF" | tar -x -C "$qlty_work/baseline"
    mkdir -p "$qlty_work/baseline/.qlty"
    cp .qlty/qlty.toml "$qlty_work/baseline/.qlty/qlty.toml"
    (cd "$qlty_work/baseline" && git init --quiet && git add -- .)
    # Disposable metadata only: native --upstream needs two public trees.
    # No repository history, credentials or dirty/untracked files are copied.
    git archive "$QLTY_BASE_REF" | tar -x -C "$qlty_work/candidate"
    mkdir -p "$qlty_work/candidate/.qlty"
    cp .qlty/qlty.toml "$qlty_work/candidate/.qlty/qlty.toml"
    (cd "$qlty_work/candidate" && git init --quiet && git add -- . &&
        git -c user.name=Public-CI -c user.email=ci@example.invalid commit --quiet -m baseline)
    comparison_base=$(git -C "$qlty_work/candidate" rev-parse HEAD)
    git -C "$qlty_work/candidate" rm --quiet -r -- .
    git archive HEAD | tar -x -C "$qlty_work/candidate"
    (cd "$qlty_work/candidate" && git add -- . &&
        git -c user.name=Public-CI -c user.email=ci@example.invalid commit --quiet -m candidate)
    for scope in baseline candidate; do
        selection=(--all)
        [[ "$scope" != candidate ]] || selection=(--upstream "$comparison_base")
        code=0
        (cd "$qlty_work/$scope" && QLTY_TELEMETRY=off "$qlty_bin" check "${selection[@]}" \
            --no-fix --no-formatters --no-cache --no-progress --print-errors \
            --sarif --jobs 2 --skip-source-fetch --no-upgrade-check) \
            >"$qlty_work/$scope.sarif" 2>"$qlty_work/$scope.stderr" || code=$?
        printf '%s\n' "$code" >"$qlty_work/$scope.exit"
    done
    python3 tools/check_qlty.py "$qlty_work/baseline.sarif" \
        "$(cat "$qlty_work/baseline.exit")" "$qlty_work/candidate.sarif" \
        "$(cat "$qlty_work/candidate.exit")"
elif [[ "$mode" == dependencies || "$mode" == firmware ]]; then
    : "${IDF_PATH:?Use the ESP-IDF 6.1 environment}"
    # The SDK lives in the disposable CI container, never a developer's SDK.
    if [[ ${CI:-} != true ]]; then
        echo 'Synthetic firmware setup is restricted to disposable CI.' >&2
        exit 2
    fi
    python3 tests/check_idf6_scope.py
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
        size=$(python -c "import csv; print(next(int(r[4].strip(),0) for r in csv.reader(open('firmware/boards/$board.partitions.csv')) if r and r[0]=='user_images'))")
        "firmware/build-$board/littlefs_py_venv/bin/python" tools/check_image_seed.py \
            --target "$board" --manifest assets/generated/manifest.json \
            --image "firmware/build-$board/user_images.bin" --size "$size"
    fi
elif [[ "$mode" == host ]]; then
    : "${IDF_PATH:?Use the ESP-IDF 6.1 runner environment}"
    mbedtls_source="${IDF_PATH}/components/mbedtls/mbedtls"
    if [[ ! -f "${mbedtls_source}/include/mbedtls/ssl.h" ||
          ! -f "${mbedtls_source}/tf-psa-crypto/include/mbedtls/md.h" ]]; then
        echo 'Pinned ESP-IDF mbedTLS 4.1.0 public headers are missing from IDF_PATH.' >&2
        exit 1
    fi
    python3 tests/check_crop_geometry.py
    python3 tests/host_private_wifi_defaults/check.py
    python3 tests/host_tooling_qlty58/test_tooling_contracts.py
    cmake -S . -B build-ci-host -G Ninja \
        -DKATMED_MBEDTLS_SOURCE="${mbedtls_source}"
    cmake --build build-ci-host -j 4
    ctest --test-dir build-ci-host --output-on-failure
    cmake -S firmware/components/katmed_ui/host -B build-ci-ui -G Ninja \
        -DKATMED_UI_HOST_MEM34=ON \
        -DKATMED_MBEDTLS_SOURCE="${mbedtls_source}"
    cmake --build build-ci-ui -j 4
    ctest --test-dir build-ci-ui --output-on-failure
    mkdir -p build-ci-ui/surface
    build-ci-ui/check_ui_surface 240 240 0 build-ci-ui/surface c3
    build-ci-ui/check_ui_surface 172 320 0 build-ci-ui/surface c6
    build-ci-ui/check_ui_surface 320 172 1 build-ci-ui/surface c6-landscape
    # Existing narrow main-contact check expects bootstrap tools at this path.
    host_bootstrap="$repo/build-ci-tools"
    mkdir -p "$host_bootstrap/bootstrap/bin"
    ln -sf "$(command -v cmake)" "$host_bootstrap/bootstrap/bin/cmake"
    ln -sf "$(command -v ninja)" "$host_bootstrap/bootstrap/bin/ninja"
    KATMED_TOOLS_DIR="$host_bootstrap" bash tests/host_c3_alarm_render/check_alarm_panel_transfer.sh "$repo/build-ci-c3-alarm-panel-transfer"
    KATMED_TOOLS_DIR="$host_bootstrap" KATMED_BOARD_CACHE_BUILD_DIR="$repo/build-ci-board-cache" KATMED_BOARD_CACHE_ONLY_BRIGHTNESS=1 bash tests/host_board_cache_review/check_board_main_ui.sh
    bash tests/host_reset_physical/check.sh
    KATMED_TOOLS_DIR="$host_bootstrap" bash tests/host_reset_physical/check_ui.sh
    KATMED_TOOLS_DIR="$host_bootstrap" bash tests/host_main/check_main_contact.sh
    bash tests/host_mesh/check_interaction.sh
    bash tests/host_mesh/check_admission.sh
    bash tests/host_store/check_store.sh
    bash tests/host_security/check.sh
    python3 tests/host_source_export/check.py
    python3 tests/host_security_migration/check.py
    python3 tests/host_security_import/check.py
    bash tests/host_config_bridge/check_bridge.sh
    bash tests/host_web_auth/check_web_auth.sh
    python3 -m unittest discover -s tests/host_web_ui -p 'test_ap_host.py'
    node tests/host_web_ui/check_async_buttons.js
    node tests/host_i18n/check_web_locale.js
    python3 tests/host_web_ui/check_contract.py
    python3 -m unittest discover -s tests/host_web_ui -p 'test_security_js_gzip.py'
    python3 -m unittest discover -s tests/host_web_ui -p 'test_security_js_handler.py'
    python3 -m unittest discover -s tests/host_web_ui -p 'test_network_startup.py'
    python3 -m unittest discover -s tests/host_web_ui -p 'test_language_owner.py'
    python3 -m unittest discover -s tests/host_web_ui -p 'test_emergency_owner.py'
    python3 tests/host_runtime_metrics/test_c3_resource_policy.py
    # This standalone main performs a real synthetic TLS handshake; discovery
    # only imports it. Build the pinned public SDK library and run it explicitly.
    native_tls_build="$repo/build-ci-native-tls"
    native_tls_config="$repo/tests/host_web_ui/native_config"
    cmake -S "$mbedtls_source" -B "$native_tls_build" -G Ninja \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF \
        -DMBEDTLS_FRAMEWORK_DIR="$mbedtls_source/framework" \
        -DCMAKE_PROJECT_INCLUDE="$native_tls_config/port.cmake" \
        -DCMAKE_C_FLAGS="-I$native_tls_config"
    cmake --build "$native_tls_build" -j 4
    KATMED_MBEDTLS_HOST_BUILD="$native_tls_build" \
        KATMED_MBEDTLS_HOST_CONFIG="$native_tls_config" \
        python3 tests/host_web_ui/test_tls_server_signature_policy.py
    # Lightweight adapter contracts only; the real HA Core fixture remains separate.
    python3 -m unittest discover -s tests/ha_integration -p 'test_adapter.py'
    bash tests/host_runtime_metrics/check_runtime_metrics.sh
    bash tests/host_store/check_main_submit.sh
    bash tests/host_mesh/check_engine.sh
    bash tests/host_mesh/check_status.sh
    bash tests/host_mesh/check_wire.sh
else
    echo 'Unknown CI action' >&2
    exit 2
fi
