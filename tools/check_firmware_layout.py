#!/usr/bin/env python3
"""Check the built app and the existing image pack against the real table.

This is an offline build gate. It makes no assertion about runtime free heap.
"""
import argparse
import csv
import hashlib
import json
import struct
from pathlib import Path


def check_layout(repo: Path, board: str, build: Path) -> dict:
    # Existing target-local sdkconfig files survive a defaults change. Verify
    # both the input and the generated header consumed by the compiler.
    config = (build / "sdkconfig").read_text()
    stack_setting = "CONFIG_ESP_MAIN_TASK_STACK_SIZE=11264"
    if stack_setting not in config.splitlines():
        raise ValueError("Target sdkconfig main-task stack differs from required 11264 bytes")
    header = (build / "config" / "sdkconfig.h").read_text()
    if "#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 11264" not in header.splitlines():
        raise ValueError("Compiled main-task stack differs from required 11264 bytes")
    if "CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=3072" not in config.splitlines():
        raise ValueError("Target sdkconfig timer-task stack differs from required 3072 bytes")
    if "#define CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH 3072" not in header.splitlines():
        raise ValueError("Compiled timer-task stack differs from required 3072 bytes")
    if "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y" not in config.splitlines():
        raise ValueError("Target sdkconfig lacks the primary USB Serial/JTAG input console")
    if "#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1" not in header.splitlines():
        raise ValueError("Compiled firmware lacks the primary USB Serial/JTAG input console")
    flash_bytes = {"c3": 0x400000, "c6": 0x800000}[board]
    rows = []
    with (repo / "firmware" / "boards" / f"{board}.partitions.csv").open() as src:
        for row in csv.reader(line for line in src if line.strip() and not line.startswith("#")):
            name, kind, subtype, offset, size, flags = (part.strip() for part in row)
            rows.append((name, int(offset, 0), int(size, 0), kind, subtype, flags))
    previous_end = 0x9000  # 0x8000 partition table occupies one 4 KiB sector.
    partitions = {}
    for name, offset, size, kind, subtype, flags in sorted(rows, key=lambda r: r[1]):
        if size <= 0 or offset < previous_end or offset + size > flash_bytes:
            raise ValueError(f"Overlapping or out-of-flash partition: {name}")
        if offset % 0x1000 or (kind == "app" and offset % 0x10000):
            raise ValueError(f"Unaligned partition: {name}")
        previous_end = offset + size
        if name in partitions:
            raise ValueError(f"Duplicate partition: {name}")
        partitions[name] = (offset, size, kind, subtype, flags)
    # ESP-IDF gen_esp32part.py uses <2sBBLL16sL and bit 1 for readonly.
    # Check the actual binary, so a stale/wrong-target CMake configuration fails.
    binary = (build / "partition_table" / "partition-table.bin").read_bytes()
    actual = {}
    for pos in range(0, len(binary) - 31, 32):
        entry = binary[pos:pos + 32]
        if entry[:2] != b"\xaa\x50":
            break  # MD5 trailer or erased flash padding follows the entries.
        _, kind, subtype, offset, size, raw_name, flags = struct.unpack("<2sBBII16sI", entry)
        name = raw_name.rstrip(b"\0").decode("ascii")
        if name in actual:
            raise ValueError(f"Duplicate binary partition: {name}")
        actual[name] = (offset, size, kind, subtype, flags)
    expected_binary = {}
    for name, (offset, size, kind, subtype, flags) in partitions.items():
        subtype_id = {"nvs": 2, "phy": 1, "factory": 0, "littlefs": 0x83}.get(subtype)
        if subtype_id is None:
            subtype_id = int(subtype, 0)
        expected_binary[name] = (offset, size, {"app": 0, "data": 1}[kind], subtype_id,
                                 2 if flags == "readonly" else 0)
    if actual != expected_binary:
        raise ValueError("Built partition table does not match board CSV")
    app = build / "katmed_foundation.bin"
    app_bytes = app.stat().st_size
    app_budget = partitions["factory"][1]
    if not 0 < app_bytes <= app_budget:
        raise ValueError(f"App size {app_bytes} exceeds budget {app_budget}")
    image_partition = partitions["images"]
    if image_partition[4] != "readonly":
        raise ValueError("Image partition must be read-only")
    generated = repo / "assets" / "generated"
    manifest = json.loads((generated / "manifest.json").read_text())
    pack_name = f"{board}_images.bin"
    expected = manifest["packs"][pack_name]
    data = (generated / pack_name).read_bytes()
    if len(data) != expected["bytes"] or hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise ValueError("Image pack differs from verified manifest")
    if len(data) > image_partition[1] or expected["budget"] != image_partition[1]:
        raise ValueError("Image pack and partition budgets disagree")
    return {"board": board, "app_bytes": app_bytes, "app_budget": app_budget,
            "app_free": app_budget - app_bytes, "image_bytes": len(data),
            "image_budget": image_partition[1], "image_free": image_partition[1] - len(data),
            "runtime_heap": "not_measured"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", choices=("c3", "c6"))
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = args.build_dir or repo / "firmware" / f"build-{args.board}"
    print(json.dumps(check_layout(repo, args.board, build), indent=2))


if __name__ == "__main__":
    main()
