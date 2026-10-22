#!/usr/bin/env python3
"""Mount an actual generated LittleFS image and verify its sole standard corpus.

Use the already required littlefs-python image-builder environment. Host-only;
no serial/device APIs, formatting, firmware or key material are involved.
"""
import argparse
import json
from pathlib import Path
from littlefs import LittleFS, UserContext
from generate_firmware_catalog import PROFILES, parse_and_verify


def check_seed(manifest, target, image, expected_size):
    parsed = parse_and_verify(manifest, target)
    if image.stat().st_size != expected_size or expected_size % 4096:
        raise ValueError('LittleFS image/partition extent mismatch')
    context = UserContext(expected_size)
    context.buffer[:] = image.read_bytes()
    fs = LittleFS(context=context, mount=False, block_size=4096,
                  block_count=expected_size // 4096, name_max=64)
    fs.mount()  # Explicit: a failed mount must never silently format a fixture.
    try:
        if set(fs.listdir('/')) != {'factory-images', 'user-images'} or fs.listdir('/user-images'):
            raise ValueError('Unexpected seed namespace or uploaded files')
        expected = {}
        for index, entry in enumerate(parsed[3]):
            for profile_index, (profile, _, _) in enumerate(PROFILES[target]):
                expected[f'cat-{index:02x}-{profile_index}.jpg'] = manifest.parent / entry['variants'][profile]['file']
        if set(fs.listdir('/factory-images')) != set(expected):
            raise ValueError('Missing, duplicated or stale standard file')
        # One image at a time; no concatenated stock package is produced.
        for name, source in expected.items():
            with fs.open('/factory-images/' + name, 'rb') as stored, source.open('rb') as original:
                while True:
                    a, b = stored.read(512), original.read(512)
                    if a != b:
                        raise ValueError('Standard JPEG content mismatch')
                    if not a:
                        break
        used = fs.used_block_count * 4096
        return {'target': target, 'logical_cats': len(parsed[3]),
                'jpeg_files': len(expected), 'partition_bytes': expected_size,
                'jpeg_payload_bytes': sum(p.stat().st_size for p in expected.values()),
                'used_bytes_including_metadata': used,
                'free_bytes_before_upload_reserve': expected_size - used,
                'all_files_exact': True, 'uploaded_files': 0}
    finally:
        fs.unmount()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=PROFILES, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--size', type=lambda s: int(s, 0), required=True)
    a = p.parse_args()
    print(json.dumps(check_seed(a.manifest.resolve(), a.target, a.image, a.size), indent=2))
