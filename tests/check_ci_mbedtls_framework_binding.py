#!/usr/bin/env python3
"""Regression checks for the pinned mbedTLS framework source binding."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools/prepare_ci_host_submodules.py"
SPEC = importlib.util.spec_from_file_location("ci_host_preflight", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("Cannot load CI host preflight helper")
PREFLIGHT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREFLIGHT)


class MbedTLSFrameworkBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="katmed-mbedtls-framework-")
        self.root = Path(self.temporary.name) / "mbedtls"
        self.source = self.root / "tf-psa-crypto/framework"
        package = self.source / "scripts/mbedtls_framework"
        package.mkdir(parents=True)
        (package / "__init__.py").write_text("# pinned fixture\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_creates_and_reuses_exact_source_symlink(self) -> None:
        PREFLIGHT.prepare_mbedtls_framework_layout(self.root)
        binding = self.root / "framework"
        self.assertTrue(binding.is_symlink())
        self.assertEqual(binding.resolve(), self.source.resolve())

        PREFLIGHT.prepare_mbedtls_framework_layout(self.root)
        self.assertTrue(binding.is_symlink())
        self.assertEqual(binding.resolve(), self.source.resolve())

    def test_rejects_symlink_to_another_source(self) -> None:
        binding = self.root / "framework"
        other = Path(self.temporary.name) / "other-framework"
        other.mkdir()
        binding.symlink_to(other, target_is_directory=True)

        with self.assertRaisesRegex(RuntimeError, "unexpected source"):
            PREFLIGHT.prepare_mbedtls_framework_layout(self.root)

    def test_rejects_any_existing_real_directory_even_if_it_has_same_named_package(self) -> None:
        binding = self.root / "framework/scripts/mbedtls_framework"
        binding.mkdir(parents=True)
        (binding / "__init__.py").write_text("# unrelated synthetic package\n", encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "not the pinned source symlink"):
            PREFLIGHT.prepare_mbedtls_framework_layout(self.root)

    def test_rejects_incomplete_pinned_source(self) -> None:
        (self.source / "scripts/mbedtls_framework/__init__.py").unlink()

        with self.assertRaisesRegex(RuntimeError, "source is incomplete"):
            PREFLIGHT.prepare_mbedtls_framework_layout(self.root)


if __name__ == "__main__":
    unittest.main(verbosity=2)
