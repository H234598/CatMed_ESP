#!/usr/bin/env python3
"""Check neutral-build markers and scan firmware artifacts for local defaults.

The private input file is deliberately supplied from outside the repository.
Each non-comment line has the form category=value. Values are never printed.
"""

from __future__ import annotations

import argparse
import json
import re
import os
import shlex
import stat
import sys
from pathlib import Path


def load_private_values(path: Path) -> list[tuple[str, bytes]]:
    values: list[tuple[str, bytes]] = []
    try:
        if stat.S_IMODE(path.stat().st_mode) & 0o077:
            raise ValueError("private values input permissions must be 0600 or stricter")
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        raise ValueError("private values input unavailable") from None
    for line in lines:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        category, separator, value = line.partition("=")
        if not separator or not re.fullmatch(r"[a-zA-Z0-9_-]{1,40}", category):
            raise ValueError("private values input format invalid")
        if not value:
            continue
        values.append((category, value.encode("utf-8")))
    if not values:
        raise ValueError("private values input contains no values")
    return values


def check(build_dir: Path, private_file: Path) -> tuple[bool, str]:
    try:
        private_values = load_private_values(private_file)
    except ValueError as error:
        return False, str(error)

    build_dir = build_dir.resolve()
    config_candidates = [build_dir / "sdkconfig", build_dir.parent / "sdkconfig"]
    config_path = next((path for path in config_candidates if path.is_file()), None)
    compile_db = build_dir / "compile_commands.json"
    if config_path is None or not compile_db.is_file():
        return False, "gift build metadata missing"
    try:
        config_text = config_path.read_text(encoding="utf-8", errors="replace")
        compile_entries = json.loads(compile_db.read_text(encoding="utf-8"))
    except OSError:
        return False, "gift build metadata unreadable"
    except (json.JSONDecodeError, TypeError):
        return False, "gift compile database invalid"
    if "CONFIG_KATMED_GIFT_BUILD=y" not in config_text:
        return False, "gift sdkconfig marker missing"
    compile_tokens: list[str] = []
    for entry in compile_entries if isinstance(compile_entries, list) else []:
        if isinstance(entry, dict) and isinstance(entry.get("arguments"), list):
            compile_tokens.extend(str(token) for token in entry["arguments"])
        elif isinstance(entry, dict) and isinstance(entry.get("command"), str):
            compile_tokens.extend(shlex.split(entry["command"]))
    if not any(token in ("-DKATMED_GIFT_BUILD", "-DKATMED_GIFT_BUILD=1")
               for token in compile_tokens):
        return False, "gift compile marker missing"

    artifacts = sorted({*build_dir.rglob("*.elf"), *build_dir.rglob("*.bin")})
    if not artifacts:
        return False, "firmware artifacts missing"
    try:
        contents = b"".join(path.read_bytes() for path in artifacts)
    except OSError:
        return False, "firmware artifact unreadable"

    found = sorted({category for category, value in private_values if value in contents})
    if found:
        return False, "private defaults present; categories=" + ",".join(found)
    return True, "neutral build marker present; private default values found=0"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path)
    args = parser.parse_args()
    private_file = os.environ.get("KATMED_GIFT_PRIVACY_INPUT")
    if not private_file:
        print("FAIL: private values input unavailable")
        return 1
    passed, message = check(args.build_dir, Path(private_file))
    print(("PASS: " if passed else "FAIL: ") + message)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
