"""Run with python3 tests/check_crop_geometry.py; no test framework required."""

import argparse
import importlib.util
import json
from pathlib import Path
import tempfile


root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_images", root / "tools/prepare_images.py")
images = importlib.util.module_from_spec(spec)
spec.loader.exec_module(images)

landscape = images.PROFILES["c6_320x172"]
portrait = images.PROFILES["c6_172x320"]
square = images.PROFILES["c3_240x240"]

# The book-stack cats occupy the top of the original; their window starts at 0.
assert images.crop_geometry([1254, 1254], landscape, [0.54, 0.23]) == {
    "resized": [320, 320], "rect": [0, 0, 320, 172]
}
assert images.crop_geometry([1254, 1254], portrait, [0.30, 0.35])["rect"] == [10, 0, 172, 320]
assert images.crop_geometry([1254, 1254], landscape, [0.5, 0.5])["rect"] == [0, 74, 320, 172]

# Focus points at the edges still produce a fully filled image without padding.
for point, expected_x in [([0, 0], 0), ([1, 1], 148)]:
    assert images.crop_geometry([1254, 1254], portrait, point)["rect"] == [expected_x, 0, 172, 320]
    assert images.crop_geometry([1254, 1254], square, point)["rect"] == [0, 0, 240, 240]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, help="Optional external original image archive")
args = parser.parse_args()
if args.source is not None:
    entries = images.collect_sources(args.source)
else:
    manifest = json.loads((root / "assets/generated/manifest.json").read_text())
    entries = [
        {"relative_path": entry["source"],
         "width_px": entry["source_dimensions"][0],
         "height_px": entry["source_dimensions"][1]}
        for entry in manifest["images"]
    ]
focus = images.load_focus(root / "assets/crop_focus.json", entries)
assert len(entries) == 72
assert {entry["relative_path"] for entry in entries} == set(focus)
for entry in entries:
    for profile in images.PROFILES.values():
        crop = images.crop_geometry([entry["width_px"], entry["height_px"]], profile, focus[entry["relative_path"]])
        x, y, width, height = crop["rect"]
        assert 0 <= x <= crop["resized"][0] - width
        assert 0 <= y <= crop["resized"][1] - height

with tempfile.TemporaryDirectory() as temporary:
    path = Path(temporary) / "invalid.json"
    for point in [None, [], [0.5], [True, 0.5], [-0.1, 0.5], [0.5, 1.1], [float("nan"), 0.5]]:
        path.write_text(json.dumps({"schema_version": 1, "focus_by_source": {"example.png": point}}))
        try:
            images.load_focus(path, [{"relative_path": "example.png"}])
        except ValueError:
            pass
        else:
            raise AssertionError(f"Invalid focus accepted: {point}")

print("PASS: book-stack regression, centred crop, edge clamping, 72 complete focus entries, 216 bounded crops, invalid-input rejection")
