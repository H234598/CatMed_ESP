#!/usr/bin/env python3
"""Keep the IDF 6.1 mbedTLS 4 and LVGL 9.6 host bindings explicit."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
workflow = (ROOT / ".github/workflows/ci.yml").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


root_cmake = (ROOT / "CMakeLists.txt").read_text()
image_host_cmake = (
    ROOT / "firmware/components/katmed_images/host/CMakeLists.txt"
).read_text()
ui_host_cmake = (
    ROOT / "firmware/components/katmed_ui/host/CMakeLists.txt"
).read_text()
image_source = (
    ROOT / "firmware/components/katmed_images/katmed_images_lvgl.c"
).read_text()
host_lv_conf = (
    ROOT / "firmware/components/katmed_images/host/lv_conf.h"
).read_text()

for name, content in (
    ("main host CMake", root_cmake),
    ("image host CMake", image_host_cmake),
):
    require(
        "tf-psa-crypto/include/mbedtls/md.h" in content,
        f"{name} must validate the pinned mbedTLS 4 md.h source",
    )
    require(
        '"${KATMED_MBEDTLS_SOURCE}/include"' in content
        and '"${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/include"' in content,
        f"{name} must pass both pinned mbedTLS public include roots",
    )
    require(
        '"${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/drivers/builtin/include"'
        in content,
        f"{name} must pass the pinned mbedTLS private_access.h include root",
    )

require(
    'target_include_directories(check_ui_refinement SYSTEM PRIVATE' in ui_host_cmake
    and '"${KATMED_MBEDTLS_SOURCE}/include"' in ui_host_cmake
    and '"${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/include"' in ui_host_cmake
    and '"${KATMED_MBEDTLS_SOURCE}/tf-psa-crypto/drivers/builtin/include"'
    in ui_host_cmake,
    "UI host image-reader target must inherit both pinned mbedTLS public include roots",
)
require(
    "PRIVATE katmed_core lvgl::lvgl tfpsacrypto)" in root_cmake
    and "PRIVATE lvgl tfpsacrypto)" in image_host_cmake
    and "PRIVATE lvgl m z tfpsacrypto)" in ui_host_cmake
    and "mbedcrypto" not in root_cmake + image_host_cmake + ui_host_cmake,
    "host image checks must link the mbedTLS 4 TF-PSA crypto target, not the removed 3.x name",
)
for name, content in (
    ("main host CMake", root_cmake),
    ("image host CMake", image_host_cmake),
    ("UI host CMake", ui_host_cmake),
):
    require(
        'target_include_directories(tfpsacrypto PUBLIC "${KATMED_MBEDTLS_SOURCE}/../port/include")'
        in content,
        f"{name} must provide the ESP-IDF mbedTLS port headers to TF-PSA crypto",
    )
    require(
        '"${KATMED_MBEDTLS_SOURCE}/../port/include"' in content
        and "build-c3/config" in content,
        f"{name} must expose ESP-IDF port headers and generated sdkconfig to dependencies",
    )
for name, content in (
    ("main host CMake", root_cmake),
    ("image host CMake", image_host_cmake),
    ("UI host CMake", ui_host_cmake),
):
    require(
        'set(MBEDTLS_FRAMEWORK_DIR "${KATMED_MBEDTLS_SOURCE}/framework")' in content,
        f"{name} must bind the runner framework symlink for mbedTLS generators",
    )

require(
    "#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565" in host_lv_conf
    and "LV_COLOR_DEPTH" not in host_lv_conf,
    "host LVGL configuration must select RGB565 without deprecated color depth",
)
require(
    workflow.count("Pillow==13.0.0") == 2 and "Pillow==11.3.0" not in workflow,
    "both public CI jobs must pin the tested Pillow 13.0.0 release",
)
require(
    "lv_obj_set_hidden(image, true)" in image_source
    and "lv_obj_set_hidden(label, false)" in image_source
    and "lv_obj_is_hidden(active_image)" in image_source
    and "lv_obj_set_hidden(image, false)" in image_source
    and "lv_obj_set_hidden(fallback_label, true)" in image_source,
    "image adapter must use LVGL 9.6 dedicated hidden-state APIs",
)
lvgl_host_sources = {
    "image adapter": image_source,
    "UI implementation": (ROOT / "firmware/components/katmed_ui/ui.c").read_text(),
    "clock implementation": (
        ROOT / "firmware/components/katmed_ui/clock_view.c"
    ).read_text(),
    "image reader test": (ROOT / "tests/check_image_reader.cpp").read_text(),
    "UI surface test": (ROOT / "tests/check_ui_surface.c").read_text(),
    "UI interaction test": (ROOT / "tests/check_ui_interaction.c").read_text(),
    "Morse test": (ROOT / "tests/check_ui_morse.c").read_text(),
    "settings values test": (
        ROOT / "tests/host_store/check_settings_values.c"
    ).read_text(),
    "main contact test": (
        ROOT / "tests/host_main/main_contact_regression.c"
    ).read_text(),
}
for name, content in lvgl_host_sources.items():
    for deprecated in ("lv_obj_add_flag(", "lv_obj_remove_flag(", "lv_obj_has_flag("):
        require(
            deprecated not in content,
            f"{name} still uses deprecated LVGL API {deprecated}",
        )

print("Host mbedTLS 4/LVGL 9.6 bindings are explicitly regression-checked.")
