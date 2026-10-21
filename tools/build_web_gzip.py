#!/usr/bin/env python3
"""Build deterministic gzip copies of the two browser scripts for a firmware build."""

import argparse
import gzip
from pathlib import Path


ASSETS = ("app.js", "i18n.js")


def build(source_dir: Path, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for name in ASSETS:
        source = (source_dir / name).read_bytes()
        encoded = gzip.compress(source, compresslevel=9, mtime=0)
        if gzip.decompress(encoded) != source:
            raise ValueError(f"gzip round-trip failed for {name}")
        (output_dir / f"{name}.gz").write_bytes(encoded)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    build(args.source_dir, args.output_dir)


if __name__ == "__main__":
    main()
