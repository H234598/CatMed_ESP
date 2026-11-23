#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
build_dir="${1:?taskprivate-build-directory-required}"
driver="${2:-${test_dir}/../../firmware/components/esp_lcd_jd9853/esp_lcd_jd9853.c}"
mkdir -p -- "${build_dir}"
{
    cat "${test_dir}/check_jd9853_transfer.c"
    # Compile the real bitmap function without the unrelated GPIO and RTOS routines.
    awk '
        /^static esp_err_t panel_jd9853_draw_bitmap\(/ && !/;$/ { ++definitions; copying = 1 }
        copying { print }
        copying && /^}$/ { copying = 0; ++complete }
        END { if (definitions != 1 || complete != 1) exit 1 }
    ' "${driver}"
} | cc -std=c11 -Wall -Wextra -Werror -x c - -o "${build_dir}/check_jd9853_transfer"
"${build_dir}/check_jd9853_transfer"
