#!/usr/bin/env python3
"""Force dormant migration APIs into an existing Security IDF 6.1 target link."""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("board", choices=("c3", "c6"))
parser.add_argument("--build-dir", type=Path)
parser.add_argument("--symbol", choices=("katmed_source_export",
                                         "katmed_import_apply_idf",
                                         "katmed_import_resume_idf",
                                         "katmed_import_abort_idf",
                                         "katmed_web_import_records_valid",
                                         "katmed_import_files_empty",
                                         "katmed_import_file_publish"),
                    default="katmed_source_export")
args = parser.parse_args()
build = args.build_dir or ROOT / "firmware" / f"build-a61-{args.board}-security"
ninja = shutil.which("ninja") or str(Path(os.environ.get(
    "KATMED_TOOLS_DIR", Path.home() / ".local/share/katmed-tools")) /
    "bootstrap/bin/ninja")
if not Path(ninja).is_file() or not (build / "katmed_foundation.elf").is_file():
    raise SystemExit("Pinned target build and ninja are required")

commands = subprocess.run(
    [ninja, "-C", str(build), "-t", "commands", "katmed_foundation.elf"],
    check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
).stdout.splitlines()
words = shlex.split(commands[-1])
if words[:2] != [":", "&&"] or words[-2:] != ["&&", ":"]:
    raise SystemExit("Unexpected IDF link command shape")
link = words[2:-2]
if "-o" not in link or sum(part.startswith("-Wl,--Map=") for part in link) != 1:
    raise SystemExit("Unexpected IDF link output options")

with tempfile.TemporaryDirectory(prefix=f"katmed-{args.board}-forced-link-") as temp:
    elf = Path(temp) / "forced.elf"
    link[link.index("-o") + 1] = str(elf)
    link = [f"-Wl,--Map={temp}/forced.map" if part.startswith("-Wl,--Map=")
            else part for part in link]
    link.insert(1, f"-Wl,-u,{args.symbol}")
    result = subprocess.run(link, cwd=build, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    if result.returncode:
        diagnostics = [line for line in result.stderr.splitlines()
                       if "undefined reference" in line or "error:" in line]
        raise SystemExit("Forced target link failed: " +
                         "; ".join(diagnostics[-5:])[:1000])
    nm = str(Path(link[0]).with_name("riscv32-esp-elf-nm"))
    symbols = subprocess.run([nm, "-g", "--defined-only", str(elf)],
                             check=True, text=True, stdout=subprocess.PIPE).stdout
    if not re.search(rf"(?m)^\S+ T {re.escape(args.symbol)}$", symbols):
        raise SystemExit(f"Forced target link omitted {args.symbol}")
print(f"{args.board}: forced IDF 6.1 link contains {args.symbol}")
