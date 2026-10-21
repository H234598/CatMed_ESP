#!/usr/bin/env bash
# Four neutral Katmed source builds; no flash, monitor, profile or device step.
set -euo pipefail
umask 077
repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
build_root="${KATMED_ACME_PAIR_ROOT:?Set an absolute external KATMED_ACME_PAIR_ROOT}"
fixture_server="${KATMED_ACME_FIXTURE_SERVER:-http://127.0.0.1:17171/directory}"
if [[ "$build_root" != /* || "$build_root" == "$repo_dir" || "$build_root" == "$repo_dir"/* ]]; then
    echo 'The ACME pair output root must be absolute and outside this worktree.' >&2
    exit 2
fi
python3 - "$fixture_server" <<'PY'
import ipaddress
import sys
from urllib.parse import urlsplit
url = urlsplit(sys.argv[1])
try:
    address = ipaddress.ip_address(url.hostname or "")
except ValueError:
    raise SystemExit("Fixture directory must use a numeric private IP address")
if (url.scheme != "http" or not (address.is_private or address.is_loopback)
        or url.username or url.password or url.port is None
        or url.path != "/directory" or url.query or url.fragment):
    raise SystemExit("Fixture directory must be a credential-free private HTTP endpoint")
PY
mkdir -p -- "$build_root"
chmod 700 -- "$build_root"
python3 "$repo_dir/tools/prepare_images.py" --verify-only >/dev/null
source "$repo_dir/tools/idf_env.sh"
export IDF_PY_BUILD_JOBS="${IDF_PY_BUILD_JOBS:-4}"
for board in c3 c6; do
    case "$board" in c3) target=esp32c3 ;; c6) target=esp32c6 ;; esac
    for variant in baseline client; do
        out="$build_root/build-a61-$board-acme-$variant"
        mkdir -p -- "$out/generated"
        chmod 700 -- "$out" "$out/generated"
        python3 "$repo_dir/tools/private_wifi_defaults.py" \
            --output "$out/generated/private_wifi_defaults.h" \
            --ui-profile-output "$out/generated/katmed_private_ui_profile.h"
        client=OFF
        if [[ "$variant" == client ]]; then client=ON; fi
        if ! (cd "$repo_dir/firmware" && idf.py -B "$out" \
            -D "IDF_TARGET=$target" -D "SDKCONFIG=$out/sdkconfig" \
            -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/$board.defaults" \
            -D KATMED_ACME_RESOURCE_PROBE=ON \
            -D "KATMED_ACME_RESOURCE_CLIENT=$client" \
            -D "KATMED_ACME_RESOURCE_SERVER=$fixture_server" build) >"$out/build.log" 2>&1; then
            echo "Build failed for $board/$variant; see $out/build.log" >&2
            exit 1
        fi
        python3 "$repo_dir/tools/check_firmware_layout.py" "$board" \
            --build-dir "$out" >>"$out/build.log" 2>&1
        test -s "$out/katmed_foundation.elf"
        test -s "$out/katmed_foundation.map"
        echo "$board/$variant: app=$(stat -c %s "$out/katmed_foundation.bin") "\
"elf=$(stat -c %s "$out/katmed_foundation.elf") "\
"map=$(stat -c %s "$out/katmed_foundation.map") bytes"
    done
done
