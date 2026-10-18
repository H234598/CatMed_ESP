#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
inc=(-Ifirmware/components/katmed_time/include -Ifirmware/components/katmed_core/include -Itests/host_time_adapter/stubs)
cc -std=c11 -Wall -Wextra -Werror -pedantic -ftrapv "${inc[@]}" -c firmware/components/katmed_time/katmed_time.c -o "$out/time.o"
cc -std=c11 -Wall -Wextra -Werror -pedantic -ftrapv "${inc[@]}" -c firmware/components/katmed_time/katmed_time_idf.c -o "$out/time_idf.o"
cc -std=c11 -Wall -Wextra -Werror -pedantic -ftrapv "${inc[@]}" -c tests/host_time_adapter/check_time.c -o "$out/check.o"
c++ -std=c++17 -Wall -Wextra -Werror -pedantic "${inc[@]}" -c firmware/components/katmed_core/katmed_core.cpp -o "$out/core.o"
c++ -ftrapv "$out/time.o" "$out/time_idf.o" "$out/check.o" "$out/core.o" -o "$out/check_time"
"$out/check_time"
