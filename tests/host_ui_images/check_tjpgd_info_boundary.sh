#!/usr/bin/env bash
# Exercise the existing C3/C6 UI and JPEG paths in the 34 KiB LVGL pool,
# then check the real TJpgD header parser at its 4096-byte allocation boundary.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
bash "${repo_dir}/tests/host_ui_images/check_ui_real_images.sh"

ui_build="${repo_dir}/build-ui-host-32"
image_build="${repo_dir}/build-ui-images"
out_dir="${ui_build}/real-images"
flags=(-std=c11 -Wall -Wextra -Werror
    "-DLV_CONF_PATH=\"${repo_dir}/firmware/components/katmed_ui/host/lv_conf_34k.h\""
    -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE
    "-I${repo_dir}/firmware/components/katmed_ui/include"
    "-I${repo_dir}/firmware/components/katmed_core/include"
    "-I${repo_dir}/firmware/components/katmed_images/include"
    "-I${repo_dir}/firmware/components/katmed_images"
    "-I${repo_dir}/firmware/managed_components/lvgl__lvgl")
cc "${flags[@]}" -c "${repo_dir}/tests/host_ui_images/check_tjpgd_info_boundary.c" \
    -o "${out_dir}/check_tjpgd_info_boundary.o"

for target in c3 c6; do
    image_objects_root="${image_build}/CMakeFiles/check_image_reader_${target}.dir${repo_dir}/firmware/components/katmed_images"
    g++ "${out_dir}/check_tjpgd_info_boundary.o" \
        "${out_dir}/catalog_${target}.o" \
        "${image_objects_root}/katmed_images_reader.c.o" \
        "${image_objects_root}/katmed_images_lvgl.c.o" \
        "${image_objects_root}/katmed_images_hash.c.o" \
        "${ui_build}/lvgl/lib/liblvgl.a" \
        "${image_build}/mbedtls/library/libmbedcrypto.a" -lm \
        -o "${out_dir}/check_tjpgd_info_boundary_${target}"
done

timeout 20s "${out_dir}/check_tjpgd_info_boundary_c3" 240 240 0 \
    "${repo_dir}/assets/generated/c3_images.bin"
timeout 20s "${out_dir}/check_tjpgd_info_boundary_c6" 172 320 0 \
    "${repo_dir}/assets/generated/c6_images.bin"
timeout 20s "${out_dir}/check_tjpgd_info_boundary_c6" 320 172 1 \
    "${repo_dir}/assets/generated/c6_images.bin"
