"""Host-only padded stream fixture for legacy bounded-reader fault injection.

Not built into firmware, shipped, or written into a device partition.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from generate_firmware_catalog import PROFILES, parse_and_verify
root = Path(__file__).resolve().parents[1]
board, output = sys.argv[1:]
manifest = root / 'assets/generated/manifest.json'
parsed = parse_and_verify(manifest, board)
with Path(output).open('wb') as stream:
    for profile, _, _ in PROFILES[board]:
        for image in parsed[3]:
            stream.write((manifest.parent / image['variants'][profile]['file']).read_bytes())
            stream.write(b'\xff' * (-stream.tell() % 4))
