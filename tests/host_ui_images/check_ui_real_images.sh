#!/usr/bin/env bash
# Exercise the real bounded image reader and UI together with the 34 KiB LVGL pool.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
source "${repo_dir}/tools/idf_env.sh"
tool_dir="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
cmake_bin="${tool_dir}/bootstrap/bin/cmake"
ninja_bin="${tool_dir}/bootstrap/bin/ninja"
ui_build="${repo_dir}/build-ui-host-32"
image_build="${repo_dir}/build-ui-images"
out_dir="${ui_build}/real-images"

"${cmake_bin}" -S "${repo_dir}/firmware/components/katmed_ui/host" \
    -B "${ui_build}" -G Ninja -DCMAKE_MAKE_PROGRAM="${ninja_bin}" \
    -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
    -DKATMED_UI_HOST_MEM34=ON
"${cmake_bin}" --build "${ui_build}" --target check_ui_surface -j 8
"${cmake_bin}" -S "${repo_dir}/firmware/components/katmed_images/host" \
    -B "${image_build}" -G Ninja -DCMAKE_MAKE_PROGRAM="${ninja_bin}" \
    -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++
"${cmake_bin}" --build "${image_build}" \
    --target check_image_reader_c3 check_image_reader_c6 -j 8
mkdir -p "${out_dir}"

flags=(-std=c11 -Wall -Wextra -Werror
    "-DLV_CONF_PATH=\"${repo_dir}/firmware/components/katmed_ui/host/lv_conf_34k.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo_dir}/firmware/components/katmed_ui/include"
    "-I${repo_dir}/firmware/components/katmed_core/include"
    "-I${repo_dir}/firmware/components/katmed_images/include"
    "-I${repo_dir}/firmware/components/katmed_images"
    "-I${repo_dir}/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "${repo_dir}/firmware/components/katmed_ui/ui.c" -o "${out_dir}/ui.o"
cc "${flags[@]}" -c "${repo_dir}/tests/host_ui_images/check_ui_real_images.c" \
    -o "${out_dir}/check_ui_real_images.o"

ui_objects_root="${ui_build}/CMakeFiles/check_ui_surface.dir${repo_dir}/firmware/components"
ui_objects=(
    "${ui_objects_root}/katmed_ui/navigation.c.o"
    "${ui_objects_root}/katmed_ui/clock_view.c.o"
    "${ui_objects_root}/katmed_ui/morse.c.o"
    "${ui_objects_root}/katmed_ui/katmed_font_10.c.o"
    "${ui_objects_root}/katmed_ui/katmed_font_12.c.o"
    "${ui_objects_root}/katmed_ui/katmed_font_14.c.o"
    "${ui_objects_root}/katmed_ui/katmed_font_26.c.o"
    "${ui_objects_root}/katmed_core/katmed_core.cpp.o")
for target in c3 c6; do
    image_objects_root="${image_build}/CMakeFiles/check_image_reader_${target}.dir${repo_dir}/firmware/components/katmed_images"
    image_objects=(
        "${image_objects_root}/katmed_images_reader.c.o"
        "${image_objects_root}/katmed_images_lvgl.c.o"
        "${image_objects_root}/katmed_images_hash.c.o")
    cc "${flags[@]}" -c "${image_build}/${target}/katmed_catalog_generated.c" \
        -o "${out_dir}/catalog_${target}.o"
    g++ "${out_dir}/check_ui_real_images.o" "${out_dir}/ui.o" \
        "${out_dir}/catalog_${target}.o" "${ui_objects[@]}" "${image_objects[@]}" \
        "${ui_build}/lvgl/lib/liblvgl.a" \
        "${image_build}/mbedtls/library/libmbedcrypto.a" -lm \
        -o "${out_dir}/check_ui_real_images_${target}"
done

cc "${flags[@]}" -DDRAW_LINES=120 -c \
    "${repo_dir}/tests/host_ui_images/check_ui_real_images.c" \
    -o "${out_dir}/check_ui_real_images_120.o"
image_objects_root="${image_build}/CMakeFiles/check_image_reader_c3.dir${repo_dir}/firmware/components/katmed_images"
image_objects=(
    "${image_objects_root}/katmed_images_reader.c.o"
    "${image_objects_root}/katmed_images_lvgl.c.o"
    "${image_objects_root}/katmed_images_hash.c.o")
g++ "${out_dir}/check_ui_real_images_120.o" "${out_dir}/ui.o" \
    "${out_dir}/catalog_c3.o" "${ui_objects[@]}" "${image_objects[@]}" \
    "${ui_build}/lvgl/lib/liblvgl.a" \
    "${image_build}/mbedtls/library/libmbedcrypto.a" -lm \
    -o "${out_dir}/check_ui_real_images_c3_120"

cc "${flags[@]}" -DDRAW_LINES=80 -c \
    "${repo_dir}/tests/host_ui_images/check_ui_real_images.c" \
    -o "${out_dir}/check_ui_real_images_80.o"
g++ "${out_dir}/check_ui_real_images_80.o" "${out_dir}/ui.o" \
    "${out_dir}/catalog_c3.o" "${ui_objects[@]}" "${image_objects[@]}" \
    "${ui_build}/lvgl/lib/liblvgl.a" \
    "${image_build}/mbedtls/library/libmbedcrypto.a" -lm \
    -o "${out_dir}/check_ui_real_images_c3_80"

timeout 20s "${out_dir}/check_ui_real_images_c3" 240 240 heap \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3" 240 240 settings_latency \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3_120" 240 240 settings_latency \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3_80" 240 240 settings_latency \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3_80" 240 240 indev_release \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3_120" 240 240 indev_release \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3" 240 240 alarm_geometry \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3_120" 240 240 alarm_geometry \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3_80" 240 240 alarm_geometry \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3" 240 240 analog_overlay \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c3_80" 240 240 analog_overlay \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c6" 172 320 heap \
    "${repo_dir}/assets/generated/c6_images.bin"
timeout 20s "${out_dir}/check_ui_real_images_c6" 320 172 heap \
    "${repo_dir}/assets/generated/c6_images.bin"
