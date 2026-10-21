#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d /tmp/katmed-names-XXXXXX)
trap 'rm -rf -- "$out"' EXIT
includes=(-I"$repo/tests/host_names/stubs" -I"$repo/firmware/components/katmed_names/include" -I"$repo/firmware/components/katmed_store/include" -I"$repo/firmware/components/katmed_core/include" -I"$repo/firmware/components/katmed_security/include" -I"$repo/tests/host_mesh/nvs_stubs")
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror "${includes[@]}" -DKATMED_NAMES_DIRECTORY="\"$out/ledger\"" -c "$repo/firmware/components/katmed_names/katmed_names_idf.c" -o "$out/idf.o"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror "${includes[@]}" -c "$repo/firmware/components/katmed_store/katmed_store_idf.c" -o "$out/store-idf.o"
c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -DKATMED_MESH_HOST "${includes[@]}" "$repo/firmware/components/katmed_names/katmed_names.cpp" "$repo/firmware/components/katmed_store/katmed_store.cpp" "$repo/firmware/components/katmed_core/katmed_core.cpp" "$repo/tests/host_names/check_store.cpp" "$out/idf.o" "$out/store-idf.o" -Wl,--wrap=write -Wl,--wrap=fsync -lz -l:libcrypto.so.3 -o "$out/check"
"$out/check"
