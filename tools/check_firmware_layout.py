#!/usr/bin/env python3
"""Check the built app and the shared image filesystem against the real table.

This is an offline build gate. It makes no assertion about runtime free heap.
"""
import argparse
import csv
import hashlib
import json
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))


def require_app_project_role(app: Path, app_bytes: int, expected: str) -> None:
    # IDF 6.1 places esp_app_desc_t at the start of the first DROM segment.
    # These offsets match esp_image_header_t, esp_image_segment_header_t and
    # esp_app_desc_t (also used by esptool image-info).
    with app.open("rb") as source:
        image_header = source.read(24)
        segment_header = source.read(8)
        descriptor = source.read(256)
    if (len(image_header) != 24 or image_header[0] != 0xE9 or
            not 1 <= image_header[1] <= 16):
        raise ValueError("Invalid ESP app image header")
    if len(segment_header) != 8 or len(descriptor) != 256:
        raise ValueError("App descriptor missing or truncated")
    _, segment_bytes = struct.unpack("<II", segment_header)
    if segment_bytes < 256 or 32 + segment_bytes > app_bytes:
        raise ValueError("App descriptor segment missing or truncated")
    if struct.unpack_from("<I", descriptor)[0] != 0xABCD5432:
        raise ValueError("App descriptor magic missing")
    if descriptor[48:80] != expected.encode("ascii").ljust(32, b"\0"):
        raise ValueError(f"App project role differs from selected image: expected {expected}")


def check_layout(repo: Path, board: str, build: Path, security_profile: bool = False,
                 app_image: str = "katmed_foundation.bin") -> dict:
    if app_image == "katmed_appmaint_c6.bin" and (board != "c6" or not security_profile):
        raise ValueError("AppStore maintenance image requires C6 security profile")
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
    required = (
        "CONFIG_SECURE_FLASH_ENC_ENABLED=y",
        "CONFIG_SECURE_FLASH_ENCRYPTION_KEY_SOURCE_EFUSES=y",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y",
        "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y",
        "CONFIG_SOC_FLASH_ENCRYPTION_XTS_AES_128=y",
        "CONFIG_NVS_ENCRYPTION=y",
        "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=y",
    )
    if security_profile:
        if "CONFIG_PARTITION_TABLE_OFFSET=0x10000" not in config.splitlines():
            raise ValueError("Security profile partition table must start at 0x10000")
        for setting in required:
            if setting not in config.splitlines():
                raise ValueError(f"Security profile missing resolved setting: {setting}")
        if "CONFIG_SECURE_BOOT=y" in config.splitlines():
            raise ValueError("Secure Boot must remain separately controlled")
    elif "CONFIG_SECURE_FLASH_ENC_ENABLED=y" in config.splitlines():
        raise ValueError("Neutral build unexpectedly enables flash encryption")
    flash_bytes = {"c3": 0x400000, "c6": 0x800000}[board]
    rows = []
    partition_file = f"{board}.security.partitions.csv" if security_profile else f"{board}.partitions.csv"
    if f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/{partition_file}"' not in config.splitlines():
        raise ValueError("Resolved partition filename differs from requested profile")
    with (repo / "firmware" / "boards" / partition_file).open() as src:
        for row in csv.reader(line for line in src if line.strip() and not line.startswith("#")):
            name, kind, subtype, offset, size, flags = (part.strip() for part in row)
            rows.append((name, int(offset, 0), int(size, 0), kind, subtype, flags))
    table_offset = 0x10000 if security_profile else 0x8000
    previous_end = table_offset + 0x1000
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
        subtype_id = {"nvs": 2, "nvs_keys": 4, "phy": 1, "factory": 0, "littlefs": 0x83}.get(subtype)
        if subtype_id is None:
            subtype_id = int(subtype, 0)
        flag_bits = {"": 0, "encrypted": 1, "readonly": 2, "encrypted:readonly": 3}
        expected_binary[name] = (offset, size, {"app": 0, "data": 1}[kind], subtype_id,
                                 flag_bits[flags])
    if actual != expected_binary:
        raise ValueError("Built partition table does not match board CSV")
    if security_profile:
        critical = {
            "factory": (0x20000, 0x216000, "app", "factory", ""),
            "nvs": (0x11000, 0x7000, "data", "nvs", ""),
            "nvs_keys": (0x18000, 0x1000, "data", "nvs_keys", "encrypted"),
            "nvs_guard": (0x19000, 0x1000, "data", "0x40", ""),
            "phy_init": (0x1a000, 0x1000, "data", "phy", ""),
            "user_images": (0x236000, flash_bytes - 0x236000, "data", "littlefs", "encrypted"),
        }
        for name, expected in critical.items():
            if partitions.get(name) != expected:
                raise ValueError(f"Security partition {name} differs from runtime contract")
        # Match the KMS3 worst case: 123 images plus all five NVS values. IDF
        # reserves one 126-entry page before reporting available_entries.
        nvs_lengths = (4065, 2152, 1112, 1288, 2)
        stage_length = 16 + sum(nvs_lengths) + 123 * 13
        required_entries = ((stage_length + 31) // 32 + 4 +
                            sum((length + 31) // 32 + 4 for length in nvs_lengths) + 24)
        available_entries = (partitions["nvs"][1] // 0x1000 - 1) * 126
        if available_entries < required_entries:
            raise ValueError("Security NVS lacks usable entries for KMS3 maximum")
    elif "nvs_keys" in partitions:
        raise ValueError("Neutral profile unexpectedly includes NVS keys")
    bootloader_bytes = (build / "bootloader" / "bootloader.bin").stat().st_size
    if bootloader_bytes > table_offset:
        raise ValueError("Bootloader overlaps partition table")
    app = build / app_image
    app_bytes = app.stat().st_size
    app_budget = partitions["factory"][1]
    if not 0 < app_bytes <= app_budget:
        raise ValueError(f"App size {app_bytes} exceeds budget {app_budget}")
    expected_project = "katmed_appmaint_c6" if app_image == "katmed_appmaint_c6.bin" else "katmed_foundation"
    require_app_project_role(app, app_bytes, expected_project)
    if "images" in partitions:
        raise ValueError("Obsolete separate image partition")
    image_partition = partitions["user_images"]
    expected_image_offset = 0x236000 if security_profile else 0x280000
    if image_partition != (expected_image_offset, flash_bytes - expected_image_offset,
                            "data", "littlefs", "encrypted"):
        raise ValueError("Shared image filesystem differs from profile contract")
    from generate_firmware_catalog import parse_and_verify
    parsed = parse_and_verify(repo / "assets/generated/manifest.json", board)
    image_bytes = parsed[2]
    if image_bytes > image_partition[1]:
        raise ValueError("Standard image files exceed filesystem budget")
    seed = build / "user_images.bin"
    if not seed.is_file() or seed.stat().st_size != image_partition[1]:
        raise ValueError("Built image filesystem size differs from partition")
    return {"board": board, "security_profile": security_profile, "app_image": app_image,
            "bootloader_bytes": bootloader_bytes,
            "app_bytes": app_bytes, "app_budget": app_budget,
            "app_free": app_budget - app_bytes, "image_bytes": image_bytes,
            "image_budget": image_partition[1], "image_free": image_partition[1] - image_bytes,
            "nvs_available_entries": available_entries if security_profile else None,
            "runtime_heap": "not_measured"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", choices=("c3", "c6"))
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--security-profile", action="store_true")
    parser.add_argument("--app-image", choices=("katmed_foundation.bin", "katmed_appmaint_c6.bin"),
                        default="katmed_foundation.bin")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = args.build_dir or repo / "firmware" / f"build-{args.board}"
    print(json.dumps(check_layout(repo, args.board, build, args.security_profile,
                                  args.app_image), indent=2))


if __name__ == "__main__":
    main()
