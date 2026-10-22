#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mktemp -p /tmp katmed-mesh-count-XXXXXX)"
trap 'python3 - "$out" <<'"'"'PY'"'"'
import pathlib,sys
p=pathlib.Path(sys.argv[1])
if p.parent != pathlib.Path("/tmp") or not p.name.startswith("katmed-mesh-count-") or p.is_symlink():
    raise SystemExit("unsafe temporary test binary")
p.unlink(missing_ok=True)
PY' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -DKATMED_MESH_HOST \
    -I"$repo/tests/host_mesh/stubs" \
    -I"$repo/firmware/components/katmed_names/include" \
    -I"$repo/firmware/components/katmed_mesh/include" \
    -I"$repo/firmware/components/katmed_store/include" \
    -I"$repo/firmware/components/katmed_core/include" \
    "$repo/firmware/components/katmed_names/katmed_names.cpp" \
    "$repo/firmware/components/katmed_names/katmed_names_wire.cpp" \
    "$repo/firmware/components/katmed_mesh/katmed_mesh_wire.cpp" \
    "$repo/firmware/components/katmed_mesh/katmed_mesh_payload.cpp" \
    "$repo/firmware/components/katmed_mesh/katmed_mesh_status.cpp" \
    "$repo/firmware/components/katmed_mesh/katmed_mesh_session.cpp" \
    "$repo/firmware/components/katmed_mesh/katmed_mesh_idf.cpp" \
    "$repo/firmware/components/katmed_store/katmed_store.cpp" \
    "$repo/firmware/components/katmed_core/katmed_core.cpp" \
    "$repo/tests/host_longrun_metrics/check_mesh_counters.cpp" \
    /lib64/libcrypto.so.3 -lz -o "$out"
"$out" send_failure
"$out" queue_overflow
