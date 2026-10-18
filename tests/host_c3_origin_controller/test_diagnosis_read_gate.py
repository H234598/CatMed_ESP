"""Synthetic, real-controller regression for bounded diagnosis-file reads."""

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("controller_gates_fixture", HERE / "test_controller_gates.py")
fixture_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture_module)


class DiagnosisReadGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.controller = Path(os.environ["C3_CONTROLLER_UNDER_TEST"])
        assert cls.controller.is_file() and not cls.controller.is_symlink()

    def run_case(self, kind, payload=b'{"first_error":"other_safe_error"}'):
        with tempfile.TemporaryDirectory(prefix="c3-diagnosis-gate-synthetic-") as folder:
            fixture = fixture_module.ControllerFixture(folder, self.controller, "normal")
            original_write = fixture_module.write_private
            original_read = Path.read_bytes
            original_open = os.open
            observed = {"diagnosis_path_reads": 0, "fd_open_attempts": 0, "swapped": False}

            def install_diagnosis(path, data):
                path = Path(path)
                if path.name != "diagnosis.json":
                    return original_write(path, data)
                if kind == "fifo":
                    os.mkfifo(path, 0o600)
                    return
                if kind == "symlink":
                    target = path.parent / "synthetic-target.json"
                    original_write(target, payload)
                    path.symlink_to(target)
                    return
                original_write(path, payload)
                if kind == "loose_mode":
                    path.chmod(0o644)

            def count_path_read(path):
                if Path(path).name == "diagnosis.json":
                    observed["diagnosis_path_reads"] += 1
                return original_read(path)

            def controlled_open(path, flags, *args, **kwargs):
                if str(path) == "diagnosis.json":
                    observed["fd_open_attempts"] += 1
                    self.assertTrue(flags & os.O_NOFOLLOW)
                    self.assertTrue(flags & os.O_NONBLOCK)
                    if kind == "exchange" and not observed["swapped"]:
                        diagnosis = fixture.run_dir / "recorder" / "diagnosis.json"
                        target = diagnosis.parent / "synthetic-target.json"
                        original_write(target, payload)
                        diagnosis.unlink()
                        diagnosis.symlink_to(target)
                        observed["swapped"] = True
                return original_open(path, flags, *args, **kwargs)

            with mock.patch.object(fixture_module, "write_private", install_diagnosis), \
                 mock.patch.object(Path, "read_bytes", count_path_read), \
                 mock.patch.object(os, "open", controlled_open):
                data, external = fixture.execute()
            return data, external, observed

    def assert_rejected_without_target_read(self, kind):
        data, external, observed = self.run_case(kind)
        self.assertEqual(observed["diagnosis_path_reads"], 0)
        self.assertEqual(data["first_error"], "diagnosis_bounds")
        self.assertEqual(external.recorder_calls, 1)
        self.assertEqual(external.write_calls, 2)
        self.assertTrue(data["cleanup_started"])
        self.assertTrue(data["safe_return"])
        return observed

    def test_01_symlink_target_is_never_read(self):
        self.assert_rejected_without_target_read("symlink")

    def test_02_fifo_is_rejected_without_blocking_read(self):
        self.assert_rejected_without_target_read("fifo")

    def test_03_wrong_mode_is_rejected_before_read(self):
        self.assert_rejected_without_target_read("loose_mode")

    def test_04_path_exchange_cannot_redirect_open(self):
        observed = self.assert_rejected_without_target_read("exchange")
        self.assertTrue(observed["swapped"])
        self.assertEqual(observed["fd_open_attempts"], 1)

    def test_05_regular_ascii_json_and_size_boundary(self):
        cases = (
            ("valid", b'{"first_error":"other_safe_error"}', None, True),
            ("at_limit", b'{"first_error":"other_safe_error"}' + b" " * (4096 - 34), None, True),
            ("over_limit", b'{"first_error":"other_safe_error"}' + b" " * 4096, "diagnosis_bounds", False),
            ("non_ascii", b'{"first_error":"\xc3\xa4"}', "diagnosis_bounds", False),
            ("invalid_json", b"{", "controller_error", False),
        )
        for kind, payload, first_error, has_diagnosis in cases:
            with self.subTest(kind=kind):
                if kind == "at_limit":
                    self.assertEqual(len(payload), 4096)
                data, external, observed = self.run_case(kind, payload)
                self.assertEqual(data["first_error"], first_error)
                self.assertEqual("diagnosis" in data, has_diagnosis)
                self.assertEqual(observed["diagnosis_path_reads"], 0)
                self.assertEqual(external.recorder_calls, 1)
                self.assertEqual(external.write_calls, 2)
                self.assertTrue(data["cleanup_started"])
                self.assertTrue(data["safe_return"])


if __name__ == "__main__":
    unittest.main()
