"""Keep the complete C6 security browser scripts within the measured app gap."""

import gzip
import tempfile
import unittest
from pathlib import Path

from tools.build_web_gzip import ASSETS, build


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/components/katmed_web/ui"
ORIGINAL_C6_OVERFLOW = 83_040
MINIMUM_NEW_RESERVE = 8_192


class SecurityScriptBudget(unittest.TestCase):
    def test_oversized_script_rejected_at_build_time(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            source.mkdir()
            (source / "app.js").write_bytes(b"x" * (128 * 1024 + 1))
            with self.assertRaisesRegex(ValueError, "bounded raw response"):
                build(source, Path(directory) / "output")

    def test_complete_scripts_fit_with_reserve(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            build(SOURCE, output)
            first = {name: (output / f"{name}.gz").read_bytes() for name in ASSETS}
            build(SOURCE, output)
            saved = 0
            for name in ASSETS:
                original = (SOURCE / name).read_bytes()
                encoded = (output / f"{name}.gz").read_bytes()
                self.assertEqual(encoded, first[name], name)
                self.assertEqual(encoded[:2], b"\x1f\x8b", name)
                self.assertEqual(encoded[4:8], b"\0\0\0\0", name)
                self.assertEqual(gzip.decompress(encoded), original, name)
                saved += len(original) - len(encoded)
            self.assertGreaterEqual(saved, ORIGINAL_C6_OVERFLOW + MINIMUM_NEW_RESERVE)


if __name__ == "__main__":
    unittest.main()
