#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp)
fixture=$(mktemp)
trap 'rm -f "$out" "$fixture"' EXIT
adapter="${KATMED_MESH_ADAPTER:-$repo/firmware/components/katmed_mesh/katmed_mesh_idf.cpp}"
sanitizers=()
if [[ "${KATMED_HOST_SANITIZE:-0}" == 1 ]]; then sanitizers=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
g++ -pthread "${sanitizers[@]}" -std=c++17 -Wall -Wextra -Werror -DKATMED_MESH_HOST \
  -I"$repo/firmware/components/katmed_ui/include" \
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
  "$adapter" \
  "$repo/firmware/components/katmed_store/katmed_store.cpp" \
  "$repo/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo/tests/host_mesh/check_engine.cpp" -l:libcrypto.so.3 -lz -o "$out"
if [[ $# -gt 0 ]]; then
    for scenario in "$@"; do "$out" "$scenario"; done
    exit 0
fi
"$out"
"$out" night_interleave
"$out" night_solo
"$out" night_copy
"$out" night_leaf
"$out" mesh_only
"$out" offline
"$out" startup_checkpoints
"$out" startup_queue_failure
"$out" startup_netif_failure
"$out" manual_source
"$out" solo_settings
"$out" fanout
"$out" time
"$out" leaf_running
"$out" leaf_replay
"$out" leaf_ack2
"$out" leaf_ack2_fail
"$out" pending
"$out" send_failure
"$out" full
"$out" cold_produce "$fixture"
"$out" cold_consume "$fixture"
"$out" leaf_distinct
"$out" leaf
"$out" leaf_shared_negative
"$out" leaf_shared_timeout
"$out" order
"$out" hello_replay
"$out" incomplete
"$out" incomparable
"$out" same_domain
"$out" same_domain_no_utc
"$out" single_unknown
"$out" late_peer
"$out" queue_overflow

"$out" status_root
"$out" status_leaf
"$out" status_fair

"$out" interaction_root
"$out" interaction_leaf

"$out" peer_rekey_context
"$out" interaction_previous

"$out" name_assignment

"$out" names_lifetime

"$out" names_hostname_retry_netif

"$out" names_leaf

"$out" names_barrier

"$out" names_lease_old
"$out" names_lease_new

"$out" names_successor

"$out" names_mixed
"$out" names_mixed_fresh

"$out" names_prepare_old
"$out" names_prepare_new

"$out" names_merge_recovery
"$out" names_merge_bank_old
"$out" names_merge_bank_new

"$out" names_retarget_recovery
"$out" names_retarget_old
"$out" names_retarget_new

"$out" names_bounded_open

"$out" names_bounded_open_corrupt
