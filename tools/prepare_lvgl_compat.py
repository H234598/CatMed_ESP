#!/usr/bin/env python3
"""Apply three source-bound LVGL 9.4.0 image allocation fixes locally.

The managed component is downloaded by ESP-IDF and excluded from Katmed Git.
Unexpected LVGL source bytes are rejected so these narrow fixes cannot drift.
"""

import argparse
import hashlib
from pathlib import Path


PATCHES = (
    (
        "src/libs/tjpgd/lv_tjpgd.c",
        "e86658188441209ca9512c1025080e18dc7c4556a5a2e825ecf5878e2ec0c1f6",
        "48350d9e338c8ad547712a612198749485fcb55977c6c64797838030b255f143",
        b"""        if(decoded == NULL) {
            decoded = lv_malloc_zeroed(sizeof(lv_draw_buf_t));
            dsc->decoded = decoded;
        }
""",
        b"""        if(decoded == NULL) {
            decoded = lv_malloc_zeroed(sizeof(lv_draw_buf_t));
            if(decoded == NULL) return LV_RESULT_INVALID;
            dsc->decoded = decoded;
        }
""",
    ),
    (
        "src/libs/tjpgd/lv_tjpgd.c",
        "48350d9e338c8ad547712a612198749485fcb55977c6c64797838030b255f143",
        "1f58363ef5eb864724f284fd439043356a4e590a09b701a0a51a2e2b96ba32aa",
        b"""            uint8_t workb[TJPGD_WORKBUFF_SIZE];
            JDEC jd;
            JRESULT rc = jd_prepare(&jd, input_func, workb, TJPGD_WORKBUFF_SIZE, &dsc->file);
""",
        b"""            uint8_t * workb = lv_malloc(TJPGD_WORKBUFF_SIZE);
            if(workb == NULL) return LV_RESULT_INVALID;
            JDEC jd;
            JRESULT rc = jd_prepare(&jd, input_func, workb, TJPGD_WORKBUFF_SIZE, &dsc->file);
            lv_free(workb);
""",
    ),
    (
        "src/widgets/image/lv_image.c",
        "b8fa1ff90d3199c417d4b3e1ae474ce0100d2b7df265c8fca83c7de658657c54",
        "fc8476ab6d503942a3b2d758eee688424f9f10f7dbb358ceeb8db87e6d596914",
        b"""            char * new_str = lv_strdup(src);
            LV_ASSERT_MALLOC(new_str);
            if(new_str == NULL) return;
""",
        b"""            char * new_str = lv_strdup(src);
            if(new_str == NULL) return;
""",
    ),
)


def prepare(lvgl_root: Path, check_only: bool = False) -> str:
    version = (lvgl_root / "lv_version.h").read_text()
    for name, value in (("MAJOR", 9), ("MINOR", 4), ("PATCH", 0)):
        if f"#define LVGL_VERSION_{name} {value}" not in version:
            raise ValueError("LVGL version differs from pinned 9.4.0")
    pending = {}
    for index, (relative, original_hash, patched_hash, old, new) in enumerate(PATCHES):
        source = lvgl_root / relative
        data = pending.get(source, source.read_bytes())
        digest = hashlib.sha256(data).hexdigest()
        later_hashes = {step[2] for step in PATCHES[index + 1:] if step[0] == relative}
        if digest in later_hashes and data.count(new) == 1:
            continue
        if digest == patched_hash and data.count(new) == 1:
            continue
        if digest == original_hash and data.count(old) == 1:
            patched = data.replace(old, new, 1)
            if hashlib.sha256(patched).hexdigest() != patched_hash:
                raise ValueError(f"LVGL patch result differs for {relative}")
            pending[source] = patched
            continue
        raise ValueError(f"Unexpected LVGL 9.4.0 source SHA-256 for {relative}: {digest}")
    if check_only:
        return f"verified LVGL 9.4.0 sources; {len(pending)} local fixes required"
    for source, patched in pending.items():
        source.write_bytes(patched)
        if hashlib.sha256(source.read_bytes()).hexdigest() != hashlib.sha256(patched).hexdigest():
            raise ValueError(f"LVGL image fix verification failed: {source}")
    return f"verified LVGL 9.4.0 image fixes; {len(pending)} newly applied"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lvgl_root", type=Path)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    print(prepare(args.lvgl_root, args.check_only))
