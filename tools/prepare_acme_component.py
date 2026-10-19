#!/usr/bin/env python3
"""Apply the reviewed atomic-write bridge to official acmeclient 1.1.1.

The component manager owns the extracted source. This bounded, idempotent
step runs in the private build tree after resolution and refuses any source
that is not the verified 1.1.1 payload. It never changes the SDK or a
component outside the current firmware build tree.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys


EXPECTED = {
    "Acme.cpp": "acaef64533ab8c194f9bdf958ccd32df3fcc3de6c00e67ab12fa05e6c398c595",
    "Cache.cpp": "7d843c8297f16eb9f0ac2761330f3c49c4093fe2c9c9e6c5962830cd0fe31ceb",
    "Crypto.cpp": "a5e479237b4a5b1560068b7f85aa568b3aaafb3c4b0aa84c39630f5d3351b9f3",
}
MARKER = "KATMED_ACME_ATOMIC_PATCH_1"
DECLARATION = """
#include <stdio.h>
extern \"C\" FILE *katmed_acme_atomic_open(const char *target);
extern \"C\" int katmed_acme_atomic_close(FILE *stream);
""".strip()


class PatchError(ValueError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _patch_source(path: Path, writes: tuple[str, ...], closes: tuple[str, ...],
                  expected_counts: dict[str, int]) -> None:
    original = path.read_text(encoding="utf-8")
    if MARKER in original:
        updated = original
        if "#include <stdio.h>" not in updated:
            updated = updated.replace("/* " + MARKER + " */",
                                      "#include <stdio.h>\n\n/* " + MARKER + " */", 1)
        updated = updated.replace('katmed_acme_atomic_open(fn, "w")',
                                  "katmed_acme_atomic_open(fn)")
        updated = updated.replace('katmed_acme_atomic_open(localfn, "w")',
                                  "katmed_acme_atomic_open(localfn)")
        if updated != original:
            path.write_text(updated, encoding="utf-8")
        return
    if not original.startswith("/*") or "#include" not in original:
        raise PatchError(f"official ACME source shape unknown: {path.name}")
    changed = original
    for old in writes:
        count = changed.count(old)
        expected = expected_counts.get(old, 1)
        if count != expected:
            raise PatchError(f"expected one write site in {path.name}")
        replacement = old.replace("fopen", "katmed_acme_atomic_open").replace(', "w"', "")
        changed = changed.replace(old, replacement, expected)
    for old in closes:
        count = changed.count(old)
        expected = expected_counts.get(old, 1)
        if count != expected:
            raise PatchError(f"expected one close site in {path.name}")
        changed = changed.replace(old, old.replace("fclose", "katmed_acme_atomic_close"), expected)
    include = '#include "esp_log.h"\n'
    if include not in changed:
        include = '#include "Acme.h"\n'
    if include not in changed:
        raise PatchError(f"ACME include anchor missing in {path.name}")
    changed = changed.replace(include, include + "\n/* " + MARKER + " */\n" + DECLARATION + "\n", 1)
    path.write_text(changed, encoding="utf-8")


def apply(component: Path) -> bool:
    component = component.resolve()
    manifest = component / "idf_component.yml"
    if not component.is_dir() or not manifest.is_file():
        raise PatchError("resolved dannybackx/acmeclient component is missing")
    manifest_text = manifest.read_text(encoding="utf-8")
    if "version: 1.1.1" not in manifest_text:
        raise PatchError("resolved ACME component is not version 1.1.1")
    paths = {name: component / name for name in EXPECTED}
    for name, path in paths.items():
        if not path.is_file():
            raise PatchError(f"official ACME source is missing: {name}")
        if MARKER not in path.read_text(encoding="utf-8") and _sha256(path) != EXPECTED[name]:
            raise PatchError(f"official ACME source checksum mismatch: {name}")
    changed = False
    before = {name: path.read_bytes() for name, path in paths.items()}
    try:
        _patch_source(paths["Cache.cpp"],
                      ('FILE *f = fopen(fn, "w");',),
                      ("fclose(f);",),
                      {'FILE *f = fopen(fn, "w");': 4, "fclose(f);": 4})
        _patch_source(paths["Crypto.cpp"],
                      ('FILE *f = fopen(fn, "w");',),
                      ("fclose(f);", "fclose(f) != 0"), {})
        _patch_source(paths["Acme.cpp"],
                      ('FILE *f = fopen(fn, "w");', 'FILE *tf = fopen(localfn, "w");'),
                      ("fclose(f);", "fclose(tf);"), {})
        changed = any(path.read_bytes() != before[name] for name, path in paths.items())
    except Exception:
        for name, path in paths.items():
            path.write_bytes(before[name])
        raise
    return changed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--component", required=True, type=Path)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    try:
        changed = apply(args.component)
    except (OSError, PatchError) as exc:
        print(f"ACME-Atomic-Patch abgebrochen: {exc}", file=sys.stderr)
        return 1
    if not args.quiet:
        print("ACME-Atomic-Patch: " + ("angewendet" if changed else "bereits aktiv"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
