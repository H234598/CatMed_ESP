#!/usr/bin/env python3
"""Initialize only the IDF 6.1 submodules required by the isolated CI host job."""

import json
import os
from pathlib import Path
import subprocess
import sys

REQUIRED = {
    "components/lwip/lwip": "lwip_commit",
    "components/mbedtls/mbedtls": "mbedtls_commit",
}
RUNNER_IDF = Path("/opt/esp/idf")


def git(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()


def runner_idf_from_environment() -> Path:
    if os.environ.get("CI") != "true":
        raise RuntimeError("Refusing to initialize IDF submodules outside CI=true")
    raw = os.environ.get("IDF_PATH", "")
    if not raw or Path(raw).resolve() != RUNNER_IDF.resolve():
        raise RuntimeError("Refusing to initialize submodules outside the official /opt/esp/idf runner SDK")
    return RUNNER_IDF.resolve()


def expected_gitlinks(idf: Path, manifest: dict) -> None:
    if git("-C", str(idf), "rev-parse", "HEAD") != manifest["idf_commit"]:
        raise RuntimeError("Runner IDF revision differs from the compatibility manifest")
    output = git("-C", str(idf), "ls-tree", "HEAD", *REQUIRED.keys())
    actual = {}
    for line in output.splitlines():
        metadata, path = line.split("\t", 1)
        mode, kind, commit = metadata.split()
        if mode != "160000" or kind != "commit":
            raise RuntimeError(f"Expected an IDF submodule gitlink at {path}")
        actual[path] = commit
    for path, manifest_key in REQUIRED.items():
        if actual.get(path) != manifest.get(manifest_key):
            raise RuntimeError(f"IDF gitlink and compatibility manifest disagree for {path}")


def unready_submodules(idf: Path, manifest: dict) -> list[str]:
    """Return only required submodules absent or not at their pinned gitlink."""
    unready = []
    for path, manifest_key in REQUIRED.items():
        checkout = idf / path
        try:
            actual = git("-C", str(checkout), "rev-parse", "HEAD")
        except subprocess.CalledProcessError:
            actual = ""
        if actual != manifest[manifest_key] or not required_sources_present(idf, path):
            unready.append(path)
    return unready


def required_sources_present(idf: Path, path: str) -> bool:
    if path == "components/mbedtls/mbedtls":
        root = idf / path
        return (
            (root / "include/mbedtls/ssl.h").is_file()
            and (root / "tf-psa-crypto/include/mbedtls/md.h").is_file()
        )
    if path == "components/lwip/lwip":
        return (idf / path / "src/core/ipv4/ip4_napt.c").is_file()
    return False


def prepare(repo: Path) -> None:
    idf = runner_idf_from_environment()
    manifest_path = repo / "firmware/compat/idf-6.1-lwip-napt-hooks.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected_gitlinks(idf, manifest)

    # Initialize only required gitlinks that are absent or at the wrong commit.
    # Never recurse into nested modules or update unrelated SDK components.
    unready = unready_submodules(idf, manifest)
    if unready:
        subprocess.run(
            ["git", "-C", str(idf), "submodule", "update", "--init", "--",
             *unready],
            check=True,
        )

    for path, manifest_key in REQUIRED.items():
        checkout = idf / path
        if not checkout.is_dir() or git("-C", str(checkout), "rev-parse", "HEAD") != manifest[manifest_key]:
            raise RuntimeError(f"Runner submodule is not at its pinned revision: {path}")

    if not required_sources_present(idf, "components/mbedtls/mbedtls"):
        raise RuntimeError("Pinned Mbed TLS 4.1.0 public headers are incomplete")
    if not required_sources_present(idf, "components/lwip/lwip"):
        raise RuntimeError("Pinned lwIP NAPT source is missing after submodule initialization")
    print("CI host SDK submodules verified: pinned lwIP and Mbed TLS 4.1.0")


if __name__ == "__main__":
    try:
        prepare(Path(__file__).resolve().parents[1])
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"CI host SDK preflight failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
