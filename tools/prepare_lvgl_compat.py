#!/usr/bin/env python3
"""Apply narrowly source-pinned compatibility guards to LVGL 9.6.0.

The managed component is downloaded by ESP-IDF and excluded from Katmed Git.
Unexpected upstream source changes fail closed rather than receiving a fuzzy
patch.
"""

import argparse
import hashlib
from pathlib import Path


PATCHES = (
    (
        "src/libs/tjpgd/tjpgd.h",
        'e0d16ca7451d57e0370adebed5d4e7104c8a77e696c137e966f7821a8f49d258',
        '2e6396c0933e62d8e84948913bd3447ea26df70cd23616611f930f03e3cd9c42',
        b'/* TJpgDec API functions */\n',
        b'/* TJpgDec API functions */\n/* ESP32-C6 ROM exports an older, incompatible JDEC ABI under these names.\n * Keep declarations, definitions and LVGL callers bound to this pinned copy. */\n#define jd_prepare katmed_lvgl_jd_prepare\n#define jd_decomp katmed_lvgl_jd_decomp\n',
    ),
    (
        'src/misc/lv_text_ap.c',
        '111d15c7c6ac956892980432b30a7c7aa46382d278855442cdc2a5064d82d689',
        '774faa46c387479967ea54be21be445a58958353adf04016fe100dddc64fa849',
        b'    ch_fin = (uint32_t *)lv_malloc(sizeof(uint32_t) * (txt_length + 1));\n\n    i = 0;',
        b"    ch_fin = (uint32_t *)lv_malloc(sizeof(uint32_t) * (txt_length + 1));\n    if(ch_enc == NULL || ch_fin == NULL) {\n        lv_free(ch_enc);\n        lv_free(ch_fin);\n        /* Keep the original UTF-8 when temporary shaping storage is unavailable. */\n        /* Callers size the destination with lv_text_ap_strlen(). */\n        if(lv_strlen(txt) > lv_text_ap_strlen(txt)) txt_out[0] = '\\0';\n        else if(txt_out != txt) lv_strcpy(txt_out, txt);\n        return;\n    }\n\n    i = 0;",
    ),
    (
        "src/core/lv_obj_style.c",
        '295d7a500a7a592032e91df9bd3341573e87f674db91f635a38af7966c37a8e9',
        '0b149a5bdb4a83df72581f72ab812932ac7d5860211e89b7e62207efcdf73326',
        b'    lv_obj_style_t * style_trans = get_trans_style(obj, part);\n    lv_style_set_prop',
        b'    lv_obj_style_t * style_trans = get_trans_style(obj, part);\n    if(style_trans == NULL) return;\n    lv_style_set_prop',
    ),
    (
        "src/image/lv_tjpgd.c",
        "3b346915201651112782c966e8bf63aff847bb31b3d7f22e076b24005ee4a7a6",
        "4a23bd84c5ea8382bb5793166a320d2b67a99ab21ca72bac45a8b4067410b23f",
        b"""        if(decoded == NULL) {
            decoded = lv_malloc_zeroed(sizeof(lv_draw_buf_t));
            dsc->decoded = decoded;
        }""",
        b"""        if(decoded == NULL) {
            decoded = lv_malloc_zeroed(sizeof(lv_draw_buf_t));
            if(decoded == NULL) return LV_RESULT_INVALID;
            dsc->decoded = decoded;
        }""",
    ),
    (
        "src/widgets/image/lv_image.c",
        "53df2b6128f69a7c53b3741594b953623bfcd648332dd95026b6987e27167608",
        "afe9365bcf71c35666b2b15f283d13345c5bbf37d798b91ea21b4dc6cf619c97",
        b"""            char * new_str = lv_strdup(src);
            LV_ASSERT_MALLOC(new_str);
            if(new_str == NULL) return;""",
        b"""            char * new_str = lv_strdup(src);
            if(new_str == NULL) return;""",
    ),
)


def prepare(lvgl_root: Path, check_only: bool = False) -> str:
    version = (lvgl_root / "include/lvgl/lv_version.h").read_text()
    for name, value in (("MAJOR", 9), ("MINOR", 6), ("PATCH", 0)):
        if f"#define LVGL_VERSION_{name} {value}" not in version:
            raise ValueError("LVGL version differs from pinned 9.6.0")

    pending = []
    for relative, original_hash, patched_hash, old, new in PATCHES:
        source = lvgl_root / relative
        data = source.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest == patched_hash and data.count(new) == 1:
            continue
        if digest != original_hash or data.count(old) != 1:
            raise ValueError(f"Unexpected LVGL 9.6.0 source SHA-256 for {relative}: {digest}")
        patched = data.replace(old, new, 1)
        if hashlib.sha256(patched).hexdigest() != patched_hash:
            raise ValueError(f"LVGL allocation patch result differs for {relative}")
        pending.append((source, patched))

    if check_only:
        return f"verified LVGL 9.6.0 source pins; {len(pending)} local allocation guards required"
    for source, patched in pending:
        source.write_bytes(patched)
        if hashlib.sha256(source.read_bytes()).hexdigest() != hashlib.sha256(patched).hexdigest():
            raise ValueError(f"LVGL allocation guard verification failed: {source}")
    return f"verified LVGL 9.6.0 allocation guards; {len(pending)} newly applied"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lvgl_root", type=Path)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    print(prepare(args.lvgl_root, args.check_only))
