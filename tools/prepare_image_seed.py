#!/usr/bin/env python3
"""Build the sole target-specific LittleFS seed from validated JPEG files."""
import argparse
from pathlib import Path
import shutil
from generate_firmware_catalog import PROFILES, parse_and_verify


def prepare(manifest, target, output):
    parsed = parse_and_verify(manifest, target)
    output = output.resolve()
    factory = output / 'factory-images'
    factory.mkdir(parents=True, exist_ok=True)
    (output / 'user-images').mkdir(exist_ok=True)
    expected = set()
    for index, image in enumerate(parsed[3]):
        for profile_index, (profile, _, _) in enumerate(PROFILES[target]):
            destination = factory / f'cat-{index:02x}-{profile_index}.jpg'
            expected.add(destination.name)
            source = manifest.parent / image['variants'][profile]['file']
            # Never merge stale files from a different target/profile into a seed.
            shutil.copyfile(source, destination)
    for path in factory.iterdir():
        if path.name not in expected:
            if not path.is_file() or path.is_symlink():
                raise ValueError('Unexpected seed entry')
            path.unlink()
    print(f'Seed {target}: 72 logical cats, {len(expected)} JPEG files')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=PROFILES, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    prepare(a.manifest.resolve(), a.target, a.output)
