#!/usr/bin/env python3
"""Bound the actual C3 main-task stack path for a history-bearing store reopen."""

import argparse
from pathlib import Path
import re
import subprocess


FUNCTIONS = {
    "app_main": "<app_main>",
    "store_open": "<store_open>",
    "decode_record": "::decode_record(",
    "valid_record": "::valid_record(",
    "app_make_snapshot": "<app_make_snapshot>",
}
RESERVE = 1024  # Leaves room for caller frames, interrupts, and compiler variation.


def frame_start(lines: list[str]) -> int:
    """Read the entry prologue, including GCC's separate 4 KiB allocation."""
    used = 0
    t0 = None
    for line in lines[:25]:
        parts = line.split("\t")
        if len(parts) < 4:
            continue
        instruction = f"{parts[-2].strip()} {parts[-1].strip()}"
        if re.match(r"^(?:b[a-z]+|j|ret)\s", instruction):
            break
        if match := re.fullmatch(r"lui\s+t0,0x([0-9a-f]+)", instruction):
            value = int(match.group(1), 16) << 12
            t0 = value - (1 << 32) if value & (1 << 31) else value
        if match := re.fullmatch(r"addi\s+sp,sp,-([0-9]+)", instruction):
            used += int(match.group(1))
        elif instruction == "add sp,sp,t0":
            if t0 is None or t0 >= 0:
                raise ValueError("unknown stack adjustment")
            used -= t0
    if not used:
        raise ValueError("missing stack frame")
    return used


def main() -> None:
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("elf", type=Path)
    args.add_argument("sdkconfig", type=Path)
    args.add_argument("objdump", type=Path)
    a = args.parse_args()
    config = a.sdkconfig.read_text()
    match = re.search(r"^CONFIG_ESP_MAIN_TASK_STACK_SIZE=([0-9]+)$", config, re.M)
    if not match:
        raise SystemExit("missing C3 main-task stack setting")
    budget = int(match.group(1))
    dump = subprocess.run([str(a.objdump), "-d", "-C", str(a.elf)],
                          check=True, capture_output=True, text=True).stdout
    blocks = re.split(r"(?=^[0-9a-f]+ <[^\n]+>:$)", dump, flags=re.M)
    frames = {}
    for key, symbol in FUNCTIONS.items():
        matches = [block for block in blocks if symbol in block.splitlines()[0]]
        if key == "decode_record" and not matches:
            # GCC may inline the decoder into store_open after the large
            # temporary disappears; its frame is then counted there.
            frames[key] = 0
            continue
        if len(matches) != 1:
            raise SystemExit(f"missing or ambiguous {key} symbol")
        frames[key] = frame_start(matches[0].splitlines()[1:])
    path = sum(frames.values())
    print(f"C3 history-reopen stack path={path} B, main={budget} B, "
          f"reserve={RESERVE} B, frames={frames}")
    if path > budget - RESERVE:
        raise SystemExit("history-reopen stack path exceeds safe main-task budget")


if __name__ == "__main__":
    main()
