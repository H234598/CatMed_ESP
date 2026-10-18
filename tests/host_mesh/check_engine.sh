#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp)
fixture=$(mktemp)
trap 'rm -f "$out" "$fixture"' EXIT
g++ -std=c++17 -Wall -Wextra -Werror -DKATMED_MESH_HOST \
  -I"$repo/tests/host_mesh/stubs" \
  -I"$repo/firmware/components/katmed_mesh/include" \
  -I"$repo/firmware/components/katmed_store/include" \
  -I"$repo/firmware/components/katmed_core/include" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_wire.cpp" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_payload.cpp" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_status.cpp" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_session.cpp" \
  "$repo/firmware/components/katmed_mesh/katmed_mesh_idf.cpp" \
  "$repo/firmware/components/katmed_store/katmed_store.cpp" \
  "$repo/firmware/components/katmed_core/katmed_core.cpp" \
  "$repo/tests/host_mesh/check_engine.cpp" -l:libcrypto.so.3 -lz -o "$out"
"$out"
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
