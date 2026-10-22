#!/usr/bin/env python3
"""Validate the individual image files and emit a target-specific, compact C index."""

import argparse
import hashlib
import json
from pathlib import Path
import unicodedata



ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "c3": (("c3_240x240", 240, 240),),
    "c6": (("c6_172x320", 172, 320), ("c6_320x172", 320, 172)),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def image_sort_key(source):
    nfc = unicodedata.normalize("NFC", source)
    return Path(nfc).name.casefold(), nfc.casefold(), source


def parse_and_verify(manifest_path, target):
    base = manifest_path.parent
    manifest = json.loads(manifest_path.read_text())
    require(manifest.get("schema_version") == 3, "Unsupported manifest schema")
    images = manifest.get("images")
    require(isinstance(images, list) and len(images) == 72 and manifest.get("image_count") == 72,
            "The current catalogue must contain exactly 72 images")
    require(len({x["source"] for x in images}) == 72 and
            [x["source"] for x in images] == sorted((x["source"] for x in images), key=image_sort_key),
            "Image ordering or source uniqueness differs from the host catalogue")
    require([x["sort_index"] for x in images] == list(range(72)), "Sort indexes are not canonical")
    logical = [(x["source"], x["source_sha256"]) for x in images]
    catalogue_sha = digest(json.dumps(logical, ensure_ascii=False, separators=(",", ":")).encode())
    require(catalogue_sha == manifest.get("catalogue_sha256"), "Logical catalogue hash mismatch")
    require(len({x["id"] for x in images}) == 72, "Logical image ID collision")
    for entry in images:
        require(entry["id"] == entry["source_sha256"][:16] and len(entry["id"]) == 16,
                "Logical image ID does not match the original")
        int(entry["id"], 16)

    profile_specs = PROFILES[target]
    # Offsets describe a virtual, padded validation stream only. No packed
    # image bytes are stored in firmware or in a second flash partition.
    stream_hash = hashlib.sha256()
    variants = [[None, None] for _ in images]
    cursor = 0
    for profile_index, (profile, width, height) in enumerate(profile_specs):
        require(manifest["profiles"][profile]["width"] == width and
                manifest["profiles"][profile]["height"] == height, "Profile dimensions mismatch")
        for index, entry in enumerate(images):
            variant = entry["variants"][profile]
            path = base / variant["file"]
            require(path.is_file(), f"Missing image: {path}")
            data = path.read_bytes()
            length = variant["bytes"]
            require(length == len(data) and length > 0 and digest(data) == variant["sha256"],
                    f"Image SHA-256/length mismatch: {path}")
            require(data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9", f"JPEG boundary mismatch: {path}")
            next_cursor = (cursor + length + 3) & ~3
            stream_hash.update(data)
            stream_hash.update(b"\xff" * (next_cursor - cursor - length))
            variants[index][profile_index] = (cursor, length, width, height, variant["sha256"])
            cursor = next_cursor
    return catalogue_sha, stream_hash.hexdigest(), cursor, images, variants


def byte_array(hex_string):
    return "{" + ",".join(f"0x{int(hex_string[i:i+2], 16):02x}" for i in range(0, 64, 2)) + "}"


def emit(path, target, parsed):
    catalogue_sha, pack_sha, pack_size, images, variants = parsed
    lines = [
        "/* Generated from the validated current image manifest. Do not edit. */",
        '#include "katmed_images_internal.h"',
        "const KatmedGeneratedCatalog katmed_generated_catalog = {",
        f"    .profile_count = {len(PROFILES[target])}, .pack_bytes = {pack_size}u,",
        f"    .catalog_sha256 = {byte_array(catalogue_sha)},",
        f"    .pack_sha256 = {byte_array(pack_sha)},",
        "    .images = {",
    ]
    for image, pair in zip(images, variants):
        var_lines = []
        for variant in pair:
            if variant is None:
                var_lines.append("{0}")
            else:
                offset, length, width, height, sha = variant
                var_lines.append("{" + f"{offset}u,{length}u,{width}u,{height}u,{byte_array(sha)}" + "}")
        lines.append("        {UINT64_C(0x" + image["id"] + "),{" + ",".join(var_lines) + "}},")
    lines += ["    }", "};", ""]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def main():
    from prepare_images import verify as verify_prepared_images
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=PROFILES, required=True)
    parser.add_argument("--manifest", type=Path, default=ROOT / "assets/generated/manifest.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text())
    verify_prepared_images(Path(manifest["source_root"]) if manifest["source_root"] else None, manifest_path.parent,
                           ROOT / "assets/crop_focus.json")
    # Every target generation validates all target files. A stale variant cannot be
    # hidden simply because this particular board selects the other profile.
    for target in PROFILES:
        parsed_target = parse_and_verify(manifest_path, target)
        if target == args.target:
            parsed = parsed_target
    emit(args.output, args.target, parsed)
    print(f"Validated {len(parsed[3])} images, {len(PROFILES[args.target])} profiles, "
          f"{parsed[2]} logical file bytes")


if __name__ == "__main__":
    main()
