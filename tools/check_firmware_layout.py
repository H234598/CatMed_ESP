#!/usr/bin/env python3
"""Check the built app and the shared image filesystem against the real table.

This is an offline build gate. It makes no assertion about runtime free heap.
"""
import argparse
import csv
import json
import re
import shlex
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

DEFAULT_APP_IMAGE = "katmed_foundation.bin"
MAINTENANCE_APP_IMAGE = "katmed_appmaint_c6.bin"


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


def require_development_build(repo: Path, build: Path, config: str, header: str, board: str) -> None:
    """Bind the distinct Development001 profile to resolved compiler inputs."""
    settings = {
        "ESP_MAIN_TASK_STACK_SIZE": "11264", "FREERTOS_TIMER_TASK_STACK_DEPTH": "3072",
        "ESP_CONSOLE_USB_SERIAL_JTAG": "y",
        "IDF_TARGET": f'"esp32{board}"',
        f"ESPTOOLPY_FLASHSIZE_{'4MB' if board == 'c3' else '8MB'}": "y",
        "ESPTOOLPY_FLASHMODE_DIO": "y", "APP_RETRIEVE_LEN_ELF_SHA": "24",
        "PARTITION_TABLE_OFFSET": "0x10000", "SECURE_FLASH_ENC_ENABLED": "y",
        "SECURE_FLASH_ENCRYPTION_KEY_SOURCE_EFUSES": "y",
        "SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT": "y",
        "SECURE_FLASH_REQUIRE_ALREADY_ENABLED": "y", "SECURE_ENABLE_SECURE_ROM_DL_MODE": "y",
        "SOC_FLASH_ENCRYPTION_XTS_AES_128": "y", "NVS_ENCRYPTION": "y",
        "NVS_SEC_KEY_PROTECT_USING_FLASH_ENC": "y",
        "PARTITION_TABLE_CUSTOM_FILENAME": f'"boards/{board}.security.partitions.csv"',
    }
    forbidden = ("SECURE_BOOT", "SECURE_FLASH_ENCRYPTION_MODE_RELEASE")
    if board == "c6":
        settings.update({
            "ESP32C6_REV_MIN_FULL": "0", "ESP32C6_REV_MAX_FULL": "99",
            "BOOTLOADER_OFFSET_IN_FLASH": "0x0", "ESPTOOLPY_NO_STUB": "y",
            "SECURE_FLASH_CHECK_ENC_EN_IN_APP": "y",
        })
        forbidden += ("SECURE_DISABLE_ROM_DL_MODE", "SECURE_INSECURE_ALLOW_DL_MODE",
                      "SECURE_BOOT_ALLOW_JTAG", "SECURE_BOOT_V1_ENABLED", "SECURE_BOOT_V2_ENABLED",
                      "SECURE_BOOT_FLASH_ENC_KEYS_BURN_TOGETHER", "SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT",
                      "SECURE_FLASH_ENCRYPTION_KEY_SOURCE_KEY_MGR", "EFUSE_VIRTUAL")
    config_lines = config.splitlines()
    profile = "old-C3" if board == "c3" else "C6 Development001"
    for name, value in settings.items():
        resolved = [line for line in config_lines if
                    line.startswith(f"CONFIG_{name}=") or line == f"# CONFIG_{name} is not set"]
        if resolved != [f"CONFIG_{name}={value}"]:
            raise ValueError(f"Resolved {profile} setting differs: {name}")
        if header_directives(header, name) != [f"#define CONFIG_{name} {1 if value == 'y' else value}"]:
            raise ValueError(f"Compiled {profile} setting differs: {name}")
    for name in forbidden:
        if any(line.startswith(f"CONFIG_{name}=") for line in config_lines) or header_directives(header, name):
            raise ValueError(f"Forbidden {profile} setting: {name}")
    require_development_compiler(repo, build, board)


def require_old_c3_build(repo: Path, build: Path, config: str, header: str) -> None:
    """Validate the C3 compiler profile from the mandatory override graph check."""
    require_development_build(repo, build, config, header, "c3")


def header_directives(header: str, name: str) -> list[str]:
    # Inspect canonical generated definitions, not a general C preprocessor.
    return re.findall(r"^[ \t]*#[ \t]*(?:define|undef)[ \t]+CONFIG_" + re.escape(name) + r"\b[^\n]*",
                      header, re.MULTILINE)


def require_development_compiler(repo: Path, build: Path, board: str) -> None:
    modes = {"KATMED_FULL_STACK": True, "KATMED_WITH_CORE": True,
             "KATMED_C3_ALARM_DIAG": board == "c3", "KATMED_APPSTORE_MAINTENANCE": False,
             "KATMED_ACME_RESOURCE_PROBE": False, "KATMED_ACME_RESOURCE_CLIENT": False}
    cache = (build / "CMakeCache.txt").read_text().splitlines()
    cache_modes = dict(modes)
    if board == "c6":
        cache_modes["KATMED_C6_DEVELOPMENT001"] = True
    for name, enabled in cache_modes.items():
        values = [line for line in cache if line.startswith(f"{name}:")]
        if values != [f"{name}:BOOL={'ON' if enabled else 'OFF'}"]:
            raise ValueError(f"Compiler CMake mode differs: {name}")
    commands = json.loads((build / "compile_commands.json").read_text())
    main = (repo / "firmware/main/main.c").resolve()
    entries = [entry for entry in commands if
               (Path(entry["directory"]) / entry["file"]).resolve() == main]
    if len(entries) != 1:
        raise ValueError("Compiler main.c binding must be unique")
    entry = entries[0]
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    require_compiler_definitions(arguments, modes)
    require_compiler_config_include(arguments, Path(entry["directory"]), build)


def require_compiler_definitions(arguments: list[str], modes: dict[str, bool]) -> None:
    definitions = []
    iterator = iter(arguments)
    for arg in iterator:
        if arg in ("-D", "-U"):
            arg += next(iterator, "")
        if arg.startswith(("-D", "-U")):
            definitions.append(arg)
    for name, enabled in modes.items():
        values = [arg for arg in definitions if arg[2:].split("=", 1)[0] == name]
        if values != [f"-D{name}={int(enabled)}"]:
            raise ValueError(f"Compiler definition differs: {name}")


def require_compiler_config_include(arguments: list[str], directory: Path, build: Path) -> None:
    # The fixed profile include may precede IDF's config, but must not shadow it.
    includes = []
    for index, arg in enumerate(arguments):
        if arg == "-I":
            includes.append(arguments[index + 1])
        elif arg.startswith("-I"):
            includes.append(arg[2:])
    generated = build / "generated"
    if includes and (directory / includes[0]).resolve() == generated.resolve():
        shadow = generated / "sdkconfig.h"
        if generated.is_symlink() or not generated.is_dir() or shadow.exists() or shadow.is_symlink():
            raise ValueError("Compiler generated sdkconfig.h binding differs")
        includes = includes[1:]
    if not includes or (directory / includes[0]).resolve() != (build / "config").resolve():
        raise ValueError("Compiler generated sdkconfig.h binding differs")


def require_development_app(app: Path, board: str) -> None:
    """Check SDK parser results explicitly; parsing alone is not validation."""
    from esptool.bin_image import LoadFirmwareImage
    from esptool.cmds import _parse_app_info
    from esptool.util import FatalError

    data = app.read_bytes()
    try:
        image = LoadFirmwareImage(f"esp32{board}", data)
    except (FatalError, struct.error, IndexError, TypeError) as error:
        raise ValueError("Invalid Development001 app container") from error
    revisions = (5, 3, 3, 199) if board == "c3" else (13, 0, 0, 99)
    if (image.chip_id, image.min_rev, image.min_rev_full, image.max_rev_full) != revisions:
        raise ValueError("Development001 app chip/revision differs")
    if image.flash_mode != 2 or image.flash_size_freq >> 4 != (2 if board == "c3" else 3):
        raise ValueError("Development001 app flash mode/size differs")
    if image.calculate_checksum() != image.checksum:
        raise ValueError("Development001 app checksum differs")
    if (not image.append_digest or len(image.stored_digest) != 32 or
            image.stored_digest != image.calc_digest or image.data_length + 32 != len(data)):
        raise ValueError("Development001 app footer missing, incomplete or different")
    if not image.segments or len(image.segments[0].data) < 256:
        raise ValueError("Development001 app descriptor missing")
    descriptor = _parse_app_info(image.segments[0].data)
    if (descriptor is None or descriptor["project_name"] != "katmed_foundation" or
            descriptor["secure_version"] != 0):
        raise ValueError("Development001 app descriptor/security version differs")


def require_c6_development_offsets(build: Path) -> None:
    # Offsets are emitted separately from the contents of the partition binary.
    args = json.loads((build / "flasher_args.json").read_text())
    expected = {"0x0": "bootloader/bootloader.bin",
                "0x10000": "partition_table/partition-table.bin",
                "0x20000": DEFAULT_APP_IMAGE, "0x236000": "user_images.bin"}
    if not isinstance(args, dict) or args.get("flash_files") != expected:
        raise ValueError("C6 Development001 generated flash offsets/files differ")
    entry = args.get("app", {})
    if not isinstance(entry, dict) or (entry.get("offset"), entry.get("file")) != ("0x20000", DEFAULT_APP_IMAGE):
        raise ValueError("C6 Development001 generated factory app entry differs")


def require_runtime_config(build: Path) -> tuple[str, str]:
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
    return config, header


def require_release_config(config: str) -> None:
    required = (
        "CONFIG_SECURE_FLASH_ENC_ENABLED=y",
        "CONFIG_SECURE_FLASH_ENCRYPTION_KEY_SOURCE_EFUSES=y",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y",
        "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y",
        "CONFIG_SOC_FLASH_ENCRYPTION_XTS_AES_128=y",
        "CONFIG_NVS_ENCRYPTION=y",
        "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=y",
    )
    if "CONFIG_PARTITION_TABLE_OFFSET=0x10000" not in config.splitlines():
        raise ValueError("Security profile partition table must start at 0x10000")
    for setting in required:
        if setting not in config.splitlines():
            raise ValueError(f"Security profile missing resolved setting: {setting}")
    if "CONFIG_SECURE_BOOT=y" in config.splitlines():
        raise ValueError("Secure Boot must remain separately controlled")


def load_partitions(repo: Path, config: str, board: str, security_profile: bool,
                    table_offset: int, flash_bytes: int) -> dict:
    rows = []
    partition_file = f"{board}.security.partitions.csv" if security_profile else f"{board}.partitions.csv"
    if f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/{partition_file}"' not in config.splitlines():
        raise ValueError("Resolved partition filename differs from requested profile")
    with (repo / "firmware" / "boards" / partition_file).open() as src:
        for row in csv.reader(line for line in src if line.strip() and not line.startswith("#")):
            name, kind, subtype, offset, size, flags = (part.strip() for part in row)
            rows.append((name, int(offset, 0), int(size, 0), kind, subtype, flags))
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
    return partitions


def require_built_partitions(build: Path, partitions: dict) -> None:
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


def require_security_partitions(partitions: dict, flash_bytes: int) -> int:
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
    return available_entries


def require_shared_images(repo: Path, build: Path, board: str, partitions: dict,
                          expected_image_offset: int, flash_bytes: int) -> tuple[int, int]:
    if "images" in partitions:
        raise ValueError("Obsolete separate image partition")
    image_partition = partitions["user_images"]
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
    return image_bytes, image_partition[1]


def require_profile_selection(board: str, security_profile: bool, app_image: str,
                              old_c3_development001: bool, c6_development001: bool) -> None:
    if old_c3_development001 and (board != "c3" or not security_profile or
                                  app_image != DEFAULT_APP_IMAGE):
        raise ValueError("The old-C3 Development001 gate requires the C3 security profile")
    if c6_development001 and (board != "c6" or not security_profile or
                             app_image != DEFAULT_APP_IMAGE):
        raise ValueError("The C6 Development001 gate requires the C6 app security profile")
    if app_image == MAINTENANCE_APP_IMAGE and (board != "c6" or not security_profile):
        raise ValueError("AppStore maintenance image requires C6 security profile")


def check_layout(repo: Path, board: str, build: Path, security_profile: bool = False,
                 app_image: str = DEFAULT_APP_IMAGE, *,
                 old_c3_development001: bool = False, c6_development001: bool = False) -> dict:
    require_profile_selection(board, security_profile, app_image, old_c3_development001, c6_development001)
    development001 = old_c3_development001 or c6_development001
    config, header = require_runtime_config(build)
    if development001:
        require_development_build(repo, build, config, header, board)
    elif security_profile:
        require_release_config(config)
    elif "CONFIG_SECURE_FLASH_ENC_ENABLED=y" in config.splitlines():
        raise ValueError("Neutral build unexpectedly enables flash encryption")
    flash_bytes = {"c3": 0x400000, "c6": 0x800000}[board]
    table_offset = 0x10000 if security_profile else 0x8000
    partitions = load_partitions(repo, config, board, security_profile, table_offset, flash_bytes)
    require_built_partitions(build, partitions)
    available_entries = None
    if security_profile:
        available_entries = require_security_partitions(partitions, flash_bytes)
    elif "nvs_keys" in partitions:
        raise ValueError("Neutral profile unexpectedly includes NVS keys")
    bootloader_bytes = (build / "bootloader" / "bootloader.bin").stat().st_size
    if bootloader_bytes > table_offset:
        raise ValueError("Bootloader overlaps partition table")
    app = build / app_image
    app_bytes = app.stat().st_size
    app_offset, app_budget = partitions["factory"][:2]
    if not 0 < app_bytes <= app_budget:
        raise ValueError(f"App size {app_bytes} exceeds budget {app_budget}")
    expected_project = "katmed_appmaint_c6" if app_image == MAINTENANCE_APP_IMAGE else "katmed_foundation"
    require_app_project_role(app, app_bytes, expected_project)
    if development001:
        require_development_app(app, board)
    if c6_development001:
        require_c6_development_offsets(build)
    image_offset = 0x236000 if security_profile else 0x280000
    image_bytes, image_budget = require_shared_images(repo, build, board, partitions,
                                                       image_offset, flash_bytes)
    app_sector_bytes = (app_bytes + 0xfff) & ~0xfff
    return {"board": board, "security_profile": security_profile, "app_image": app_image,
            "old_c3_development001": old_c3_development001,
            "c6_development001": c6_development001,
            "bootloader_bytes": bootloader_bytes,
            "app_bytes": app_bytes, "app_budget": app_budget,
            "app_sector_end": app_offset + app_sector_bytes,
            "app_sector_free": app_budget - app_sector_bytes,
            "app_free": app_budget - app_bytes, "image_bytes": image_bytes,
            "image_budget": image_budget, "image_free": image_budget - image_bytes,
            "nvs_available_entries": available_entries,
            "runtime_heap": "not_measured"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", choices=("c3", "c6"))
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--security-profile", action="store_true")
    profiles = parser.add_mutually_exclusive_group()
    profiles.add_argument("--old-c3-development001", action="store_true",
                        help="Require the distinct old-C3 Development001 security contract")
    profiles.add_argument("--c6-development001", action="store_true",
                          help="Require the distinct C6 Development001 app security contract")
    parser.add_argument("--app-image", choices=(DEFAULT_APP_IMAGE, MAINTENANCE_APP_IMAGE),
                        default=DEFAULT_APP_IMAGE)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = args.build_dir or repo / "firmware" / f"build-{args.board}"
    print(json.dumps(check_layout(repo, args.board, build, args.security_profile,
                                  args.app_image, old_c3_development001=args.old_c3_development001,
                                  c6_development001=args.c6_development001), indent=2))


if __name__ == "__main__":
    main()
