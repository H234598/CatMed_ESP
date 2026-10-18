#!/usr/bin/env python3
"""Apply one hash-bound IoT-Bridge DNS hook fix only to a Katmed-owned SDK."""
import argparse
import fcntl
import hashlib
from pathlib import Path
import subprocess

IDF_COMMIT = "b774170ff46c393eeb5e495ea37936038d3f4f4f"
LWIP_COMMIT = "fd432e4ee2cfb7f7f1c7eb7227e0173412e7b84e"
BEFORE = "3435a579954121bc7401f44949c573a7018a6fdcded757773b580d5b41183964"
AFTER = "c35b3bdb2c2054affc708437f78cd76a900bc8d4bd8eacc8b98a9a9aa93c365a"


def source_state(source: Path) -> str:
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest == BEFORE:
        return "needs_patch"
    if digest == AFTER:
        return "patched"
    raise ValueError("Unknown DHCP source hash; refusing to change this SDK")


def prepare(sdk: Path, repo: Path, apply: bool) -> str:
    sdk, repo = sdk.resolve(), repo.resolve()
    marker = sdk / ".katmed-owned-sdk"
    if not marker.is_file() or marker.read_text().strip() != str(repo):
        raise ValueError("SDK is not explicitly owned by this Katmed checkout")
    lwip = sdk / "components/lwip/lwip"
    for directory, expected in ((sdk, IDF_COMMIT), (lwip, LWIP_COMMIT)):
        actual = subprocess.check_output(["git", "-C", str(directory), "rev-parse", "HEAD"], text=True).strip()
        if actual != expected:
            raise ValueError("SDK/submodule revision differs from the reviewed compatibility patch")
    source = lwip / "src/core/ipv4/dhcp.c"
    if not source.resolve().is_relative_to(sdk):
        raise ValueError("DHCP source escapes the owned SDK")
    with (sdk / ".katmed-compat.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        state = source_state(source)
        if state == "needs_patch" and apply:
            patch = repo / "firmware/compat/idf-5.5.5-iot-bridge-dhcp.patch"
            subprocess.run(["git", "apply", "--check", str(patch)], cwd=lwip, check=True)
            subprocess.run(["git", "apply", str(patch)], cwd=lwip, check=True)
            state = source_state(source)
            if state != "patched":
                raise ValueError("Applied patch did not produce the reviewed source")
        return state


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    print("IoT-Bridge DHCP compatibility:", prepare(args.sdk, repo, args.apply))


if __name__ == "__main__":
    main()
