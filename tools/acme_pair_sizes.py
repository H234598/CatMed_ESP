#!/usr/bin/env python3
"""Read actual ELF/map/bin sizes for the four neutral IDF 6.1 ACME builds."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def sdkconfig_values(path: Path) -> set[str]:
    return {line for line in path.read_text().splitlines()
            if line.startswith("CONFIG_") or line.startswith("# CONFIG_")}


def common_cache_values(path: Path) -> dict[str, str]:
    cache = path.joinpath("CMakeCache.txt").read_text().splitlines()
    wanted = {"IDF_TARGET", "KATMED_FULL_STACK", "KATMED_WITH_CORE",
              "KATMED_ACME_RESOURCE_PROBE", "KATMED_ACME_RESOURCE_SERVER"}
    values = {}
    for line in cache:
        if ":" in line and "=" in line:
            key = line.split(":", 1)[0]
            if key in wanted:
                values[key] = line.split("=", 1)[1]
    if set(values) != wanted:
        raise ValueError("CMake experiment configuration is incomplete")
    return values


def describe(path: Path, board: str, variant: str) -> dict:
    expected = {
        "KATMED_ACME_RESOURCE_PROBE:BOOL": "ON",
        "KATMED_ACME_RESOURCE_CLIENT:BOOL": "ON" if variant == "client" else "OFF",
        "IDF_TARGET:STRING": "esp32" + board,
    }
    cache = path.joinpath("CMakeCache.txt").read_text()
    for key, value in expected.items():
        resolved = [line.split("=", 1)[1] for line in cache.splitlines()
                    if line.startswith(f"{key}=")]
        if resolved != [value]:
            raise ValueError(f"{board}/{variant}: CMake contract differs: {key}")
    artifacts = {ext: path / f"katmed_foundation.{ext}" for ext in ("bin", "elf", "map")}
    if any(not item.is_file() or item.stat().st_size == 0 for item in artifacts.values()):
        raise ValueError(f"{board}/{variant}: ELF/map/bin artifact missing")
    result = subprocess.run(
        [sys.executable, "-m", "esp_idf_size", "--format", "json2", str(artifacts["map"])],
        check=True, text=True, capture_output=True,
    )
    layout = json.loads(result.stdout)["layout"]
    symbols = subprocess.run(
        ["riscv32-esp-elf-nm", "-C", str(artifacts["elf"])],
        check=True, text=True, capture_output=True,
    ).stdout
    real_path = all(re.search(rf"(?m)^\S+ T Acme::{name}\(", symbols)
                    for name in ("GenerateAccountKey", "CreateNewAccount",
                                 "AcmeProcess", "RenewCertificate"))
    if real_path != (variant == "client"):
        raise ValueError(f"{board}/{variant}: real client reachability symbols differ")
    if variant == "client" and not re.search(r"(?m)^\S+ T katmed_acme_probe_mark$", symbols):
        raise ValueError(f"{board}/client: real phase observer is missing")
    memory = {region["name"]: {"used": region["used"], "parts": region["parts"]}
              for region in layout if region["name"] in ("DRAM", "DIRAM")}
    expected_region = "DRAM" if board == "c3" else "DIRAM"
    if expected_region not in memory:
        raise ValueError(f"{board}/{variant}: no {expected_region} map region")
    return {
        "app_bytes": artifacts["bin"].stat().st_size,
        "elf_bytes": artifacts["elf"].stat().st_size,
        "map_bytes": artifacts["map"].stat().st_size,
        "real_client_symbols": real_path,
        "static_memory_bytes": memory,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    report: dict = {"schema": 1, "source": "IDF ELF/map/bin; no runtime RAM", "targets": {}}
    for board in ("c3", "c6"):
        builds = {variant: args.root / f"build-a61-{board}-acme-{variant}"
                  for variant in ("baseline", "client")}
        configs = {variant: sdkconfig_values(path / "sdkconfig")
                   for variant, path in builds.items()}
        if configs["baseline"] != configs["client"]:
            raise ValueError(f"{board}: sdkconfig differs between paired variants")
        if common_cache_values(builds["baseline"]) != common_cache_values(builds["client"]):
            raise ValueError(f"{board}: common CMake options differ between variants")
        report["targets"][board] = {
            "sdkconfig_equal": True,
            "common_cmake_equal": True,
            "baseline": describe(builds["baseline"], board, "baseline"),
            "client": describe(builds["client"], board, "client"),
        }
        base = report["targets"][board]["baseline"]
        client = report["targets"][board]["client"]
        report["targets"][board]["app_delta_bytes"] = client["app_bytes"] - base["app_bytes"]
        for name in set(base["static_memory_bytes"]) | set(client["static_memory_bytes"]):
            report["targets"][board][f"{name.lower()}_used_delta_bytes"] = (
                client["static_memory_bytes"].get(name, {}).get("used", 0) -
                base["static_memory_bytes"].get(name, {}).get("used", 0))
    print(json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"ACME pair size qualification failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
