#!/usr/bin/env python3
"""Exercise target index generation against current and damaged real assets."""

import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools/generate_firmware_catalog.py"
ASSETS = ROOT / "assets/generated"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(manifest, target, output, should_pass):
    completed = subprocess.run([
        "/usr/bin/python3", str(GENERATOR), "--target", target,
        "--manifest", str(manifest), "--output", str(output),
    ], capture_output=True, text=True)
    check((completed.returncode == 0) == should_pass,
          f"Unexpected generator result for {target}: exit {completed.returncode}, "
          f"stderr tail {completed.stderr[-500:]}")
    check(output.exists() == should_pass, "Invalid catalogue was emitted or valid catalogue omitted")


def main():
    with tempfile.TemporaryDirectory(prefix="katmed-catalog-test-") as temp:
        stage = Path(temp) / "assets"
        shutil.copytree(ASSETS, stage)
        manifest = stage / "manifest.json"
        c3 = Path(temp) / "c3.c"
        c6 = Path(temp) / "c6.c"
        run(manifest, "c3", c3, True)
        run(manifest, "c6", c6, True)
        ids_c3 = re.findall(r"\{UINT64_C\(0x([0-9a-f]{16})\)", c3.read_text())
        ids_c6 = re.findall(r"\{UINT64_C\(0x([0-9a-f]{16})\)", c6.read_text())
        check(len(ids_c3) == len(ids_c6) == 75 and ids_c3 == ids_c6,
              "Target indexes do not have the same complete logical IDs")
        for target_file in (c3, c6):
            check(".catalog_sha256 = " in target_file.read_text(), "Full catalogue hash missing")

        one_jpeg = next(stage.glob("c3_240x240/*.jpg"))
        original_jpeg = one_jpeg.read_bytes()
        one_jpeg.unlink()
        run(manifest, "c3", Path(temp) / "missing.c", False)
        one_jpeg.write_bytes(original_jpeg)

        pack = stage / "c6_images.bin"
        original_pack = pack.read_bytes()
        pack.write_bytes(bytes([original_pack[0] ^ 1]) + original_pack[1:])
        run(manifest, "c3", Path(temp) / "bad-other-target-pack.c", False)
        pack.write_bytes(original_pack)

        original_manifest = manifest.read_text()
        data = json.loads(original_manifest)
        data["images"][0]["variants"]["c3_240x240"]["offset"] = 0xffffffff
        manifest.write_text(json.dumps(data))
        run(manifest, "c3", Path(temp) / "bad-offset.c", False)
        manifest.write_text(original_manifest)

        data = json.loads(original_manifest)
        data["catalogue_sha256"] = "0" * 64
        manifest.write_text(json.dumps(data))
        run(manifest, "c6", Path(temp) / "bad-catalogue.c", False)
        manifest.write_text(original_manifest)

        data = json.loads(original_manifest)
        data["crop_config_sha256"] = "0" * 64
        manifest.write_text(json.dumps(data))
        run(manifest, "c3", Path(temp) / "stale-focus.c", False)
        manifest.write_text(original_manifest)

        data = json.loads(original_manifest)
        data["source_root"] = str(Path(temp) / "missing-originals")
        manifest.write_text(json.dumps(data))
        run(manifest, "c3", Path(temp) / "missing-source.c", False)

    print("firmware catalogue: both targets and six damaged/stale input cases passed")


if __name__ == "__main__":
    main()
