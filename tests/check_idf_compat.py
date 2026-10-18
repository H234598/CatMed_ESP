#!/usr/bin/env python3
"""Check rejection and repeatability of the bounded SDK compatibility step."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
SDK = (Path.home() / '.local/share/katmed-tools/esp-idf-v5.5.5')
spec = importlib.util.spec_from_file_location('compat', REPO / 'tools/prepare_idf_compat.py')
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)


class CompatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=REPO / 'build-host')
        self.sdk = Path(self.temp.name)
        self.lwip = self.sdk / 'components/lwip/lwip'
        self.source = self.lwip / 'src/core/ipv4/dhcp.c'
        self.source.parent.mkdir(parents=True)
        # Read only the pinned original, even when the real checkout is patched.
        self.original = subprocess.check_output(['git', '-C', str(SDK / 'components/lwip/lwip'),
            'show', compat.LWIP_COMMIT + ':src/core/ipv4/dhcp.c'])
        self.source.write_bytes(self.original)
        (self.sdk / '.katmed-owned-sdk').write_text(str(REPO))
        subprocess.run(['git', 'init', '-q', str(self.lwip)], check=True)

    def tearDown(self):
        self.temp.cleanup()

    def revisions(self):
        return patch.object(compat.subprocess, 'check_output', side_effect=[
            compat.IDF_COMMIT + '\n', compat.LWIP_COMMIT + '\n'])

    def test_unowned_sdk_rejected_without_source_change(self):
        (self.sdk / '.katmed-owned-sdk').unlink()
        with self.assertRaisesRegex(ValueError, 'not explicitly owned'):
            compat.prepare(self.sdk, REPO, True)
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_wrong_revision_rejected_without_source_change(self):
        with patch.object(compat.subprocess, 'check_output', return_value='wrong\n'):
            with self.assertRaisesRegex(ValueError, 'revision differs'):
                compat.prepare(self.sdk, REPO, True)
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_modified_source_rejected_without_source_change(self):
        self.source.write_bytes(self.original + b'\n/* foreign edit */\n')
        before = self.source.read_bytes()
        with self.revisions(), self.assertRaisesRegex(ValueError, 'Unknown DHCP source'):
            compat.prepare(self.sdk, REPO, True)
        self.assertEqual(self.source.read_bytes(), before)

    def test_check_apply_and_repeat_are_hash_exact(self):
        with self.revisions():
            self.assertEqual(compat.prepare(self.sdk, REPO, False), 'needs_patch')
        self.assertEqual(self.source.read_bytes(), self.original)
        with self.revisions():
            self.assertEqual(compat.prepare(self.sdk, REPO, True), 'patched')
        after = self.source.read_bytes()
        with self.revisions():
            self.assertEqual(compat.prepare(self.sdk, REPO, True), 'patched')
        self.assertEqual(self.source.read_bytes(), after)


if __name__ == '__main__':
    (REPO / 'build-host').mkdir(exist_ok=True)
    unittest.main()
