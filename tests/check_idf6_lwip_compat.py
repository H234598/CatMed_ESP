#!/usr/bin/env python3
"""Verify the checked-in IDF 6.1 lwIP patch against its pinned upstream blob."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPO / "firmware/compat/idf-6.1-lwip-napt-hooks.json"
MANIFEST = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
PATCH = MANIFEST_PATH.parent / MANIFEST["patch"]
SPEC = importlib.util.spec_from_file_location("iot_bridge_compat", REPO / "tools/prepare_iot_bridge_compat.py")
COMPAT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPAT)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> None:
    idf = COMPAT._validate_idf_scope(Path(os.environ["IDF_PATH"]))
    check(
        COMPAT._validate_idf_scope(Path("/opt/esp/idf"), "true") == Path("/opt/esp/idf"),
        "Official disposable runner path was not accepted",
    )
    try:
        COMPAT._validate_idf_scope(Path("/tmp/not-an-idf-runner"), "true")
    except ValueError:
        pass
    else:
        raise RuntimeError("Arbitrary CI paths must not be accepted as runner SDKs")
    check(MANIFEST["schema"] == 1, "Unsupported patch manifest schema")
    check(hashlib.sha256(PATCH.read_bytes()).hexdigest() == MANIFEST["patch_sha256"], "Patch hash mismatch")
    lwip = idf / "components/lwip/lwip"
    idf_commit = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
    lwip_commit = subprocess.check_output(["git", "-C", str(lwip), "rev-parse", "HEAD"], text=True).strip()
    check(idf_commit == MANIFEST["idf_commit"], "IDF revision mismatch")
    check(lwip_commit == MANIFEST["lwip_commit"], "lwIP revision mismatch")
    upstream = subprocess.check_output(
        ["git", "-C", str(lwip), "show", f"{lwip_commit}:{MANIFEST['source']}"],
    )
    check(hashlib.sha256(upstream).hexdigest() == MANIFEST["source_sha256_before"], "Upstream lwIP source hash mismatch")

    with tempfile.TemporaryDirectory(prefix="katmed-idf6-lwip-patch-") as tmp:
        scratch = Path(tmp)
        source = scratch / MANIFEST["source"]
        source.parent.mkdir(parents=True)
        source.write_bytes(upstream)
        subprocess.run(["git", "-C", str(scratch), "apply", "--check", str(PATCH)], check=True)
        subprocess.run(["git", "-C", str(scratch), "apply", str(PATCH)], check=True)
        result = hashlib.sha256(source.read_bytes()).hexdigest()
        check(result == MANIFEST["source_sha256_after"], "Patch output hash mismatch")
        subprocess.run(["git", "-C", str(scratch), "apply", "--reverse", "--check", str(PATCH)], check=True)
    COMPAT._prepare_idf6_napt(idf, check_only=False)
    print("IDF 6.1 lwIP compatibility patch: manifest, source, apply and reverse-check passed")


if __name__ == "__main__":
    main()
