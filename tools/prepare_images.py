#!/usr/bin/env python3
"""Prepare the complete cat-image archive for the two Katmed displays.

ImageMagick performs deterministic scaling and JPEG encoding. The originals are
read-only inputs. Python only manages the catalogue, checksums and flash packs.
"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import unicodedata

from PIL import Image


PROFILES = {
    "c3_240x240": {"width": 240, "height": 240, "quality": 70},
    "c6_172x320": {"width": 172, "height": 320, "quality": 90},
    "c6_320x172": {"width": 320, "height": 172, "quality": 90},
}
PACKS = {
    "c3_images.bin": {"profiles": ["c3_240x240"], "budget": 0x140000},
    "c6_images.bin": {
        "profiles": ["c6_172x320", "c6_320x172"], "budget": 0x540000
    },
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def name_key(path):
    normalized = unicodedata.normalize("NFC", path)
    return Path(normalized).name.casefold(), normalized.casefold(), path


def load_focus(path, entries):
    config = json.loads(path.read_text())
    if config["schema_version"] != 1:
        raise ValueError("Unsupported crop configuration version")
    focus = config["focus_by_source"]
    for entry in entries:
        point = focus.get(entry["relative_path"])
        if not isinstance(point, list) or len(point) != 2 or any(
            isinstance(v, bool) or not isinstance(v, (int, float))
            or not math.isfinite(v) or not 0 <= v <= 1 for v in point
        ):
            raise ValueError(f"Missing or invalid crop focus: {entry['relative_path']}")
    return focus


def crop_geometry(source_size, profile, focus):
    width, height = profile["width"], profile["height"]
    scale = max(width / source_size[0], height / source_size[1])
    resized = [math.floor(n * scale + 0.5) for n in source_size]
    offsets = [max(0, min(total - size, math.floor(total * point - size / 2 + 0.5)))
               for total, size, point in zip(resized, [width, height], focus)]
    return {"resized": resized, "rect": offsets + [width, height]}


def collect_sources(source):
    entries = []
    for path in source.rglob("*.png"):
        if not path.resolve().is_relative_to(source):
            raise ValueError(f"Source escapes the selected folder: {path}")
        with Image.open(path) as original:
            width, height = original.size
        entries.append({
            "relative_path": path.relative_to(source).as_posix(),
            "width_px": width, "height_px": height, "sha256": sha256(path),
        })
    entries.sort(key=lambda x: name_key(x["relative_path"]))
    if not entries:
        raise ValueError("The selected source folder contains no PNG images")
    ids = [entry["sha256"][:16] for entry in entries]
    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate image IDs; resolve before building the catalogue")
    return entries


def convert_image(job):
    source, destination, profile, crop = job
    destination.parent.mkdir(parents=True, exist_ok=True)
    scaled_width, scaled_height = crop["resized"]
    x, y, width, height = crop["rect"]
    subprocess.run([
        "magick", str(source), "-auto-orient", "-colorspace", "sRGB",
        "-filter", "Lanczos", "-resize", f"{scaled_width}x{scaled_height}!",
        "-gravity", "NorthWest", "-crop", f"{width}x{height}+{x}+{y}", "+repage",
        "-background", "white", "-alpha", "remove",
        "-alpha", "off", "-strip", "-sampling-factor", "4:2:0",
        "-interlace", "none", "-quality", str(profile["quality"]), str(destination),
    ], check=True, capture_output=True)


def build(source, output, crop_config):
    if output.exists():
        raise ValueError("Output already exists; verify it or choose a new output directory")
    entries = collect_sources(source)
    focus = load_focus(crop_config, entries)
    focus_hash = sha256(crop_config)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".katmed-images-", dir=output.parent) as temporary:
        stage = Path(temporary) / "generated"
        stage.mkdir()
        jobs = []
        for entry in entries:
            relative = Path(entry["relative_path"])
            for name, profile in PROFILES.items():
                crop = crop_geometry([entry["width_px"], entry["height_px"]], profile, focus[entry["relative_path"]])
                jobs.append((source / relative, stage / name / relative.with_suffix(".jpg"), profile, crop))
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(convert_image, jobs))
        images = []
        for index, entry in enumerate(entries):
            variants = {}
            for name in PROFILES:
                relative = Path(name) / Path(entry["relative_path"]).with_suffix(".jpg")
                path = stage / relative
                crop = crop_geometry([entry["width_px"], entry["height_px"]], PROFILES[name], focus[entry["relative_path"]])
                variants[name] = {"file": relative.as_posix(), "bytes": path.stat().st_size, "sha256": sha256(path), "crop": crop}
            images.append({
                "id": entry["sha256"][:16], "sort_index": index,
                "source": entry["relative_path"], "source_sha256": entry["sha256"],
                "source_dimensions": [entry["width_px"], entry["height_px"]],
                "focus": focus[entry["relative_path"]],
                "variants": variants,
            })
        packs = {}
        for pack_name, spec in PACKS.items():
            path = stage / pack_name
            with path.open("wb") as stream:
                for profile in spec["profiles"]:
                    for entry in images:
                        variant = entry["variants"][profile]
                        variant["pack"] = pack_name
                        variant["offset"] = stream.tell()
                        stream.write((stage / variant["file"]).read_bytes())
                        stream.write(b"\xff" * (-stream.tell() % 4))
            if path.stat().st_size > spec["budget"]:
                raise ValueError(f"Asset partition budget exceeded: {pack_name}")
            packs[pack_name] = {
                **spec, "bytes": path.stat().st_size, "sha256": sha256(path),
                "remaining_bytes": spec["budget"] - path.stat().st_size,
            }
        logical_catalogue = [(x["source"], x["source_sha256"]) for x in images]
        catalogue_hash = hashlib.sha256(json.dumps(logical_catalogue, ensure_ascii=False, separators=(",", ":")).encode()).hexdigest()
        manifest = {
            "schema_version": 2, "source_root": str(source), "image_count": len(images),
            "catalogue_sha256": catalogue_hash,
            "crop_config_sha256": focus_hash,
            "encoder": subprocess.check_output(["magick", "-version"], text=True).splitlines()[0],
            "resize": "Lanczos cover, per-image focal point, bounded crop; proportional with pixel rounding",
            "jpeg": "baseline, sRGB, 4:2:0, no metadata",
            "sort": "NFC/casefold basename, then relative path; original path breaks ties",
            "profiles": PROFILES, "packs": packs, "images": images,
        }
        (stage / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        verify(source, stage, crop_config)
        stage.rename(output)


def verify(source, output, crop_config):
    manifest = json.loads((output / "manifest.json").read_text())
    images = manifest["images"]
    # Public prepared assets can be verified without the external originals.
    entries = collect_sources(source) if source is not None else [
        {"relative_path": item["source"], "sha256": item["source_sha256"]}
        for item in images
    ]
    focus = None
    if manifest["schema_version"] >= 2:
        focus = load_focus(crop_config, entries)
        if sha256(crop_config) != manifest["crop_config_sha256"]:
            raise ValueError("Crop configuration changed since the build")
    if len(images) != len(entries) or manifest["image_count"] != len(entries):
        raise ValueError("Image count mismatch")
    expected = set()
    totals = Counter()
    for original, entry in zip(entries, images):
        if original["sha256"] != entry["source_sha256"] or original["relative_path"] != entry["source"]:
            raise ValueError("Source catalogue mismatch")
        for profile, variant in entry["variants"].items():
            if focus is not None:
                point = focus[entry["source"]]
                expected_crop = crop_geometry(entry["source_dimensions"], PROFILES[profile], point)
                if entry["focus"] != point or variant["crop"] != expected_crop:
                    raise ValueError(f"Crop geometry mismatch: {entry['source']}")
            path = output / variant["file"]
            expected.add(path)
            if path.stat().st_size != variant["bytes"] or sha256(path) != variant["sha256"]:
                raise ValueError(f"Output checksum mismatch: {path}")
            with Image.open(path) as decoded:
                decoded.load()
                spec = PROFILES[profile]
                if decoded.format != "JPEG" or decoded.size != (spec["width"], spec["height"]) or decoded.info.get("progressive"):
                    raise ValueError(f"Incorrect JPEG format or dimensions: {path}")
            with (output / variant["pack"]).open("rb") as pack:
                pack.seek(variant["offset"])
                if pack.read(variant["bytes"]) != path.read_bytes():
                    raise ValueError(f"Pack contents mismatch: {path}")
            totals[profile] += variant["bytes"]
    if expected != set(output.rglob("*.jpg")) or len(expected) != len(entries) * len(PROFILES):
        raise ValueError("Incomplete or unexpected output images")
    for name, pack in manifest["packs"].items():
        path = output / name
        if sha256(path) != pack["sha256"] or path.stat().st_size != pack["bytes"] or pack["bytes"] > pack["budget"]:
            raise ValueError(f"Pack checksum or size mismatch: {name}")
    print(json.dumps({"verified_source_images": len(entries) if source is not None else None, "verified_prepared_images": len(entries), "verified_jpegs": len(expected), "jpeg_bytes_by_profile": totals, "packs": manifest["packs"]}, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=None, help="External original archive; required only when generating images")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "assets/generated")
    parser.add_argument("--crop-config", type=Path, default=Path(__file__).resolve().parents[1] / "assets/crop_focus.json")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if not args.verify_only and args.source is None:
        parser.error("--source is required to generate images")
    if not args.verify_only and not shutil.which("magick"):
        parser.error("ImageMagick (magick) is required to generate images")
    source = args.source.resolve() if args.source is not None else None
    output = args.output.resolve()
    if source is not None and output.is_relative_to(source):
        parser.error("Output must be outside the source archive")
    if args.verify_only:
        verify(source, output, args.crop_config.resolve())
    else:
        build(source, output, args.crop_config.resolve())


if __name__ == "__main__":
    main()
