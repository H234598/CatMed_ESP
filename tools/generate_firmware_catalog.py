#!/usr/bin/env python3
"""Validate the complete image packs and emit a target-specific, compact C index."""

import argparse
import hashlib
import json
from pathlib import Path
import unicodedata

from prepare_images import verify as verify_prepared_images


ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "c3": (("c3_240x240", 240, 240),),
    "c6": (("c6_172x320", 172, 320), ("c6_320x172", 320, 172)),
}
PACK = {"c3": "c3_images.bin", "c6": "c6_images.bin"}
BUDGET = {"c3": 0x140000, "c6": 0x540000}


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
    require(manifest.get("schema_version") == 2, "Unsupported manifest schema")
    images = manifest.get("images")
    require(isinstance(images, list) and len(images) == 75 and manifest.get("image_count") == 75,
            "The current catalogue must contain exactly 75 images")
    require(len({x["source"] for x in images}) == 75 and
            [x["source"] for x in images] == sorted((x["source"] for x in images), key=image_sort_key),
            "Image ordering or source uniqueness differs from the host catalogue")
    require([x["sort_index"] for x in images] == list(range(75)), "Sort indexes are not canonical")
    logical = [(x["source"], x["source_sha256"]) for x in images]
    catalogue_sha = digest(json.dumps(logical, ensure_ascii=False, separators=(",", ":")).encode())
    require(catalogue_sha == manifest.get("catalogue_sha256"), "Logical catalogue hash mismatch")
    require(len({x["id"] for x in images}) == 75, "Logical image ID collision")
    for entry in images:
        require(entry["id"] == entry["source_sha256"][:16] and len(entry["id"]) == 16,
                "Logical image ID does not match the original")
        int(entry["id"], 16)

    profile_specs = PROFILES[target]
    pack_name = PACK[target]
    pack_meta = manifest["packs"][pack_name]
    require(pack_meta["profiles"] == [p[0] for p in profile_specs], "Pack profile list mismatch")
    require(pack_meta["budget"] == BUDGET[target], "Pack budget mismatch")
    pack_path = base / pack_name
    require(pack_path.is_file(), f"Missing image pack: {pack_path}")
    pack_bytes = pack_path.read_bytes()
    require(len(pack_bytes) == pack_meta["bytes"] and len(pack_bytes) <= BUDGET[target],
            "Image pack size or budget mismatch")
    require(digest(pack_bytes) == pack_meta["sha256"], "Image pack SHA-256 mismatch")
    variants = [[None, None] for _ in images]
    cursor = 0
    for profile_index, (profile, width, height) in enumerate(profile_specs):
        require(manifest["profiles"][profile]["width"] == width and
                manifest["profiles"][profile]["height"] == height, "Profile dimensions mismatch")
        for index, entry in enumerate(images):
            variant = entry["variants"][profile]
            path = base / variant["file"]
            require(path.is_file(), f"Missing image: {path}")
            require(variant["pack"] == pack_name and variant["offset"] == cursor and cursor % 4 == 0,
                    f"Image offset/order mismatch: {path}")
            data = path.read_bytes()
            length = variant["bytes"]
            require(length == len(data) and length > 0 and digest(data) == variant["sha256"],
                    f"Image SHA-256/length mismatch: {path}")
            require(cursor + length <= len(pack_bytes) and pack_bytes[cursor:cursor + length] == data,
                    f"Image differs from packed bytes: {path}")
            require(data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9", f"JPEG boundary mismatch: {path}")
            next_cursor = (cursor + length + 3) & ~3
            require(pack_bytes[cursor + length:next_cursor] == b"\xff" * (next_cursor - cursor - length),
                    f"Pack padding mismatch: {path}")
            variants[index][profile_index] = (cursor, length, width, height, variant["sha256"])
            cursor = next_cursor
    require(cursor == len(pack_bytes), "Trailing or omitted image bytes in pack")
    return catalogue_sha, pack_meta["sha256"], len(pack_bytes), images, variants


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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=PROFILES, required=True)
    parser.add_argument("--manifest", type=Path, default=ROOT / "assets/generated/manifest.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text())
    verify_prepared_images(Path(manifest["source_root"]) if manifest["source_root"] else None, manifest_path.parent,
                           ROOT / "assets/crop_focus.json")
    # Every target generation validates both packs. A stale variant cannot be
    # hidden simply because this particular board selects the other profile.
    for target in PROFILES:
        parsed_target = parse_and_verify(manifest_path, target)
        if target == args.target:
            parsed = parsed_target
    emit(args.output, args.target, parsed)
    print(f"Validated {len(parsed[3])} images, {len(PROFILES[args.target])} profiles, "
          f"{parsed[2]} pack bytes; catalogue SHA-256 {parsed[0]}")


if __name__ == "__main__":
    main()
