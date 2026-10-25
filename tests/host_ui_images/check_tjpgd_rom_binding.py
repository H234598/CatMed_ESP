#!/usr/bin/env python3
"""Exercise real pinned TJpgD with ESP32-C6's absolute ROM symbol collision.

No device, ROM emulation, private image or framebuffer output is involved.
The linker must retain the matching LVGL implementation before decoding.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path


def check(repo: Path) -> None:
    lvgl = repo / "firmware/managed_components/lvgl__lvgl"
    with tempfile.TemporaryDirectory(prefix="katmed-tjpgd-link-") as temporary:
        root = Path(temporary)
        for name in ("tjpgd.c", "tjpgd.h", "tjpgdcnf.h"):
            data = (lvgl / "src/libs/tjpgd" / name).read_text()
            # Preserve the real implementation; relocate only this temporary include.
            data = data.replace('"../../lvgl_public.h"',
                                '"' + str(lvgl / "src/lvgl_public.h") + '"')
            (root / name).write_text(data)
        (root / "probe.c").write_text(r'''
#include "tjpgd.h"
#include <stdio.h>
static unsigned tiles;
static size_t input(JDEC *jd, uint8_t *buffer, size_t count) {
    FILE *file = jd->device;
    if(buffer) return fread(buffer, 1, count, file);
    return fseek(file, (long)count, SEEK_CUR) == 0 ? count : 0;
}
static int output(JDEC *jd, void *pixels, JRECT *rect) {
    (void)jd; (void)pixels;
    if(rect->right < rect->left || rect->bottom < rect->top) return 0;
    ++tiles;
    return 1;
}
int main(int argc, char **argv) {
    if(argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if(!file) return 3;
    JDEC decoder;
    union { void *alignment; uint8_t bytes[4096]; } pool;
    JRESULT result = jd_prepare(&decoder, input, pool.bytes, sizeof(pool.bytes), file);
    if(result != JDR_OK) { fclose(file); return 4; }
    result = jd_decomp(&decoder, output, 0);
    int closed = fclose(file);
    return result == JDR_OK && tiles > 0 && closed == 0 ? 0 : 5;
}
''')
        executable = root / "probe"
        subprocess.run([
            "cc", "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections",
            "-DLV_KCONFIG_IGNORE", "-DLV_LVGL_H_INCLUDE_SIMPLE",
            '-DLV_CONF_PATH="' + str(repo / "firmware/components/katmed_images/host/lv_conf.h") + '"',
            "-I" + str(lvgl), str(root / "probe.c"), str(root / "tjpgd.c"),
            "-no-pie", "-Wl,--gc-sections", "-Wl,--defsym,jd_prepare=0x40000108",
            "-Wl,--defsym,jd_decomp=0x4000010c", "-o", str(executable),
        ], check=True, capture_output=True)
        symbols = subprocess.run(["nm", str(executable)], check=True,
                                 capture_output=True, text=True).stdout
        definitions = {parts[2]: parts[1] for line in symbols.splitlines()
                       if len(parts := line.split()) == 3}
        if any(definitions.get(name) not in ("T", "t") for name in
               ("katmed_lvgl_jd_prepare", "katmed_lvgl_jd_decomp")):
            raise AssertionError("ROM assignments displaced matching LVGL TJpgD code")
        # The incompatible ROM addresses remain present, but must never be called.
        for geometry in ("c6_172x320", "c6_320x172"):
            fixture = next(iter(sorted((repo / "assets/generated" / geometry).glob("*.jpg"))))
            subprocess.run([str(executable), str(fixture)], check=True,
                           capture_output=True)
        print("PASS: local TJpgD survives absolute ROM aliases; both C6 geometries decode")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path,
                        default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    check(args.repo.resolve())
