#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
source "${repo_dir}/tools/idf_env.sh"
tool_dir="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
cmake_bin="${tool_dir}/bootstrap/bin/cmake"
ninja_bin="${tool_dir}/bootstrap/bin/ninja"
image_build="${repo_dir}/build-ui-images"
out_dir="${repo_dir}/build-image-metrics"

mkdir -p "${image_build}"
python3 "${repo_dir}/tests/image_stream_fixture.py" c3 "${image_build}/c3_image_stream_fixture.bin"
python3 "${repo_dir}/tests/image_stream_fixture.py" c6 "${image_build}/c6_image_stream_fixture.bin"

"${cmake_bin}" -S "${repo_dir}/firmware/components/katmed_images/host" \
    -B "${image_build}" -G Ninja -DCMAKE_MAKE_PROGRAM="${ninja_bin}" \
    -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++
"${cmake_bin}" --build "${image_build}" \
    --target check_image_reader_c3 check_image_reader_c6 -j 8
mkdir -p "${out_dir}"

flags=(-std=c11 -Wall -Wextra -Werror
    "-DLV_CONF_PATH=\"${repo_dir}/firmware/components/katmed_images/host/lv_conf.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo_dir}/firmware/components/katmed_core/include"
    "-I${repo_dir}/firmware/components/katmed_images/include"
    "-I${repo_dir}/firmware/components/katmed_images"
    "-I${repo_dir}/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "${repo_dir}/tests/host_image_metrics/check_image_metrics.c" \
    -o "${out_dir}/check_image_metrics.o"

for target in c3 c6; do
    image_objects_root="${image_build}/CMakeFiles/check_image_reader_${target}.dir${repo_dir}/firmware/components/katmed_images"
    cc "${flags[@]}" -c "${image_build}/${target}/katmed_catalog_generated.c" \
        -o "${out_dir}/catalog_${target}.o"
    g++ "${out_dir}/check_image_metrics.o" "${out_dir}/catalog_${target}.o" \
        "${image_objects_root}/katmed_images_reader.c.o" \
        "${image_objects_root}/katmed_images_lvgl.c.o" \
        "${image_objects_root}/katmed_images_hash.c.o" \
        "${image_build}/lvgl/lib/liblvgl.a" \
        "${image_build}/mbedtls/library/libmbedcrypto.a" -lm \
        -o "${out_dir}/check_image_metrics_${target}"
done

# Compare the same real C3 JPEG with a deeper partial-render buffer. This is
# diagnostic evidence for decoder work per render area, not a timing proxy.
cc "${flags[@]}" -DDRAW_LINES=120 -c \
    "${repo_dir}/tests/host_image_metrics/check_image_metrics.c" \
    -o "${out_dir}/check_image_metrics_120.o"
image_objects_root="${image_build}/CMakeFiles/check_image_reader_c3.dir${repo_dir}/firmware/components/katmed_images"
g++ "${out_dir}/check_image_metrics_120.o" "${out_dir}/catalog_c3.o" \
    "${image_objects_root}/katmed_images_reader.c.o" \
    "${image_objects_root}/katmed_images_lvgl.c.o" \
    "${image_objects_root}/katmed_images_hash.c.o" \
    "${image_build}/lvgl/lib/liblvgl.a" \
    "${image_build}/mbedtls/library/libmbedcrypto.a" -lm \
    -o "${out_dir}/check_image_metrics_c3_120"

# Exercise the C3 fast-path buffer size against the actual image pack. The
# fixed threshold allows at most two full-image decode passes; the device boot
# log must separately prove that its DMA gate actually selected this path.
c3_lines="$(awk '/^#define KATMED_C3_FAST_DRAW_LINES / { print $3; exit }' \
    "${repo_dir}/firmware/components/board/include/board_display_budget.h")"
test -n "${c3_lines}"
cc "${flags[@]}" "-DDRAW_LINES=${c3_lines}" -c \
    "${repo_dir}/tests/host_image_metrics/check_image_metrics.c" \
    -o "${out_dir}/check_image_metrics_selected.o"
g++ "${out_dir}/check_image_metrics_selected.o" "${out_dir}/catalog_c3.o" \
    "${image_objects_root}/katmed_images_reader.c.o" \
    "${image_objects_root}/katmed_images_lvgl.c.o" \
    "${image_objects_root}/katmed_images_hash.c.o" \
    "${image_build}/lvgl/lib/liblvgl.a" \
    "${image_build}/mbedtls/library/libmbedcrypto.a" -lm \
    -o "${out_dir}/check_image_metrics_c3_selected"

timeout 30s "${out_dir}/check_image_metrics_c3" 240 240 \
    "${image_build}/c3_image_stream_fixture.bin"
timeout 30s "${out_dir}/check_image_metrics_c3_120" 240 240 \
    "${image_build}/c3_image_stream_fixture.bin"
timeout 30s "${out_dir}/check_image_metrics_c3_selected" 240 240 \
    "${image_build}/c3_image_stream_fixture.bin" 800
timeout 30s "${out_dir}/check_image_metrics_c6" 172 320 \
    "${image_build}/c6_image_stream_fixture.bin"
timeout 30s "${out_dir}/check_image_metrics_c6" 320 172 \
    "${image_build}/c6_image_stream_fixture.bin"
