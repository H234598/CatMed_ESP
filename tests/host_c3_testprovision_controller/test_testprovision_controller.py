"""Synthetische Kontrolle des tatsächlichen privaten Controllerpfads, ohne Port."""

import ast
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import time
import types
import unittest


ROOT = Path(__file__).resolve().parents[2]
OLD_FIXTURE = ROOT / "tests/host_c3_origin_controller/test_controller_gates.py"
SPEC = importlib.util.spec_from_file_location("old_controller_fixture", OLD_FIXTURE)
old = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(old)
CONTROLLER = Path(os.environ["C3_CONTROLLER_UNDER_TEST"])
HOST_SOURCE = ROOT / "tools/katmed_usb_setup.py"


class External(old.FakeExternal):
    def __init__(self, fixture, scenario):
        super().__init__(fixture, scenario, False)
        self.provision_calls = 0

    def run(self, argv, **kwargs):
        if argv[0] == "git" and "rev-parse" in argv and str(self.fixture.host) == str(argv[2]):
            head = "225024e173fc8bc3b95696b60755e0c767306aec" if self.fixture.new else old.HOST_HEAD
            self.calls.append(("run", tuple(str(x) for x in argv)))
            return types.SimpleNamespace(returncode=0, stdout=head + "\n", stderr="")
        return super().run(argv, **kwargs)

    def Popen(self, argv, **kwargs):
        if "provision" in argv:
            self.provision_calls += 1
            self.calls.append(("provision", tuple(str(x) for x in argv)))
            owner = self

            class Process:
                pid = 45678

                def wait(self, timeout=None):
                    owner.fixture.clock.seconds += 1
                    return 1 if owner.scenario == "reject_provision" else 0

            return Process()
        process = super().Popen(argv, **kwargs)
        if self.scenario == "deadline" and "read-flash" in argv:
            owner = self

            class TimedProcess:
                pid = process.pid

                def wait(self, timeout=None):
                    rc = process.wait()
                    if owner.read_calls == 1:
                        owner.fixture.clock.seconds = 890
                    return rc

            return TimedProcess()
        return process


class Fixture(old.ControllerFixture):
    def __init__(self, root, scenario):
        super().__init__(root, CONTROLLER, scenario)
        self.new = "testprovision-controller-20261015" in str(CONTROLLER)
        self.external = External(self, scenario)
        self.local = self.host / ".local"
        self.local.mkdir(mode=0o700)
        self.config = self.local / "c3-testprovision.json"
        config = {"group": "1" * 32, "secret": "2" * 64, "ssid": "SyntheticOffline",
                  "password": "OnlyTest123", "time_source": "offline.invalid"}
        if scenario == "invalid_config":
            config.pop("time_source")
        if scenario != "missing_config":
            old.write_private(self.config, json.dumps(config).encode("ascii"))
        if scenario == "loose_config":
            self.config.chmod(0o644)
        if scenario == "symlink_config":
            self.config.unlink()
            self.config.symlink_to(self.factory_path)
        host_py = self.host / "tools/katmed_usb_setup.py"
        host_bytes = HOST_SOURCE.read_bytes()
        host_py.write_bytes(host_bytes)
        self.source_fixtures[host_py] = (host_bytes, old.digest(host_bytes))
        if self.new:
            expected_parts = (
                "firmware/build-c3-km1-phasen-final/bootloader/bootloader.bin",
                "firmware/build-c3-km1-phasen-final/partition_table/partition-table.bin",
                "firmware/build-c3-km1-phasen-final/katmed_foundation.bin",
                "assets/generated/c3_images.bin",
            )
            parts = []
            for (offset, part, size, digest), relative in zip(self.parts, expected_parts):
                target = self.firm / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                old.write_private(target, part.read_bytes())
                parts.append((offset, target, size, digest))
            self.parts = parts
            self.artifacts = [p for _, p, _, _ in parts]
            remaining = (
                "firmware/build-c3-km1-phasen-final/katmed_foundation.elf",
                "firmware/build-c3-km1-phasen-final/bootloader/bootloader.elf",
                "firmware/build-c3-km1-phasen-final/project_description.json",
                "firmware/build-c3-km1-phasen-final/sdkconfig",
                "firmware/build-c3-km1-phasen-final/config/sdkconfig.h",
                "firmware/build-c3-km1-phasen-final/flash_args",
                "firmware/build-c6-km1-phasen-final/katmed_foundation.bin",
                "firmware/build-c6-km1-phasen-final/katmed_foundation.elf",
                "firmware/build-c6-km1-phasen-final/bootloader/bootloader.bin",
                "firmware/build-c6-km1-phasen-final/bootloader/bootloader.elf",
                "firmware/build-c6-km1-phasen-final/partition_table/partition-table.bin",
                "firmware/build-c6-km1-phasen-final/project_description.json",
                "firmware/build-c6-km1-phasen-final/sdkconfig",
                "firmware/build-c6-km1-phasen-final/config/sdkconfig.h",
                "firmware/build-c6-km1-phasen-final/flash_args",
                "assets/generated/c6_images.bin",
            )
            for index, relative in enumerate(remaining):
                target = self.firm / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.exists():
                    old.write_private(target, f"SYNTHETIC_ARTIFACT_{index}".encode())
                self.artifacts.append(target)

    def execute(self):
        tree = ast.parse(CONTROLLER.read_text(), filename=str(CONTROLLER))
        assert isinstance(tree.body[-1], ast.Try)
        definitions = ast.Module(body=[node for node in tree.body[:-1]
            if not (isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "START" for t in node.targets))], type_ignores=[])
        action = ast.Module(body=[tree.body[-1]], type_ignores=[])
        entries = [{"path": str(p), "size": p.stat().st_size,
                    "sha256": self.source_fixtures[p][1] if p in self.source_fixtures else old.digest(p.read_bytes())}
                   for p in getattr(self, "artifacts", [])]
        if self.scenario == "missing_artifact" and entries:
            entries.pop()
        scope = {"__name__": "synthetic_controller", "START": {
            "port": str(self.port), "t0_monotonic_ns": 0, "artifacts": entries}}
        exec(compile(definitions, str(CONTROLLER), "exec"), scope)
        scope.update(RUN=self.run_dir, HOST=self.host, FIRM=self.firm,
                     FACTORY=self.factory_path, EXPECTED_FACTORY=old.digest(self.factory),
                     BINS=self.parts, BLOCK=self.run_dir / old.NEW_BLOCK_NAME,
                     CONFIG=self.config, PORT=str(self.port), ESP="/synthetic/no-esptool",
                     time=types.SimpleNamespace(monotonic_ns=self.clock.monotonic_ns),
                     subprocess=self.external)
        actual_sha = scope["sha"]

        def bounded_sha(path):
            path = Path(path)
            if path in self.source_fixtures:
                data, expected = self.source_fixtures[path]
                if path.read_bytes() == data:
                    return expected
            return actual_sha(path)

        scope["sha"] = bounded_sha
        env_before = os.environ.copy()
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                exec(compile(action, str(CONTROLLER), "exec"), scope)
        finally:
            os.environ.clear()
            os.environ.update(env_before)
        return scope["D"], self.external


class TestProvisionController(unittest.TestCase):
    def case(self, scenario="normal"):
        with tempfile.TemporaryDirectory(prefix="c3-testprovision-synthetic-") as folder:
            fixture = Fixture(folder, scenario)
            result = fixture.execute()
            self.assertFalse(list(fixture.run_dir.glob("*.partial")))
            for image in fixture.run_dir.glob("*.bin"):
                self.assertEqual(stat.S_IMODE(image.lstat().st_mode), 0o600)
                self.assertEqual(image.stat().st_size, old.IMAGE_SIZE)
            return result

    def test_01_config_failures_precede_all_device_operations(self):
        for scenario in ("missing_config", "invalid_config", "loose_config", "symlink_config"):
            with self.subTest(scenario=scenario):
                data, external = self.case(scenario)
                self.assertEqual(data["first_error"], "config_invalid")
                self.assertEqual(external.write_calls, 0)
                self.assertEqual(external.provision_calls, 0)
                self.assertEqual(external.id_calls, 0)
                self.assertIsNone(data["t0_monotonic_ns"])

    def test_02_full_artifact_gate_precedes_device_operations(self):
        data, external = self.case("missing_artifact")
        self.assertEqual(data["first_error"], "artifact_gate")
        self.assertEqual(external.write_calls, 0)
        self.assertEqual(external.id_calls, 0)

    def test_03_wrong_preflash_has_no_write_or_restore(self):
        data, external = self.case("different_prewrite")
        self.assertEqual(data["first_error"], "prewrite_mismatch")
        self.assertEqual(external.write_calls, 0)
        self.assertFalse(data["cleanup_started"])

    def test_04_rejection_after_write_restores_entire_image(self):
        data, external = self.case("reject_provision")
        self.assertEqual(data["first_error"], "provision_rejected_or_uncertain")
        self.assertEqual(external.write_calls, 2)
        self.assertEqual(external.read_calls, 3)
        self.assertEqual(external.provision_calls, 1)
        self.assertTrue(data["safe_return"])
        self.assertEqual(external.recorder_calls, 0)

    def test_05_accepted_provision_cannot_claim_unproved_boot_or_mesh(self):
        data, external = self.case("normal")
        self.assertEqual(external.provision_calls, 1)
        self.assertTrue(data["provision_accepted"])
        self.assertEqual(data["first_error"], "postboot_unqualified")
        self.assertEqual(external.recorder_calls, 0)
        self.assertEqual(external.write_calls, 2)
        self.assertTrue(data["safe_return"])

    def test_06_deadline_after_write_still_restores(self):
        data, external = self.case("deadline")
        self.assertEqual(data["first_error"], "time_gate")
        self.assertEqual(external.provision_calls, 0)
        self.assertEqual(external.write_calls, 2)
        self.assertTrue(data["cleanup_started"])
        self.assertTrue(data["safe_return"])
        cleanup = [step for step in data["steps"] if step["name"] == "cleanup_start"]
        self.assertEqual(len(cleanup), 1)
        self.assertTrue(cleanup[0]["before_t15"])

    def test_07_parent_exit_does_not_cancel_synthetic_restore(self):
        with tempfile.TemporaryDirectory(prefix="c3-testprovision-parent-") as folder:
            result = Path(folder) / "result.json"
            launcher = subprocess.run(
                [os.sys.executable, __file__, "--synthetic-parent", folder],
                env=os.environ.copy(), capture_output=True, timeout=5)
            self.assertEqual(launcher.returncode, 0)
            deadline = time.monotonic() + 5
            while not result.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(result.exists())
            value = json.loads(result.read_text())
            self.assertEqual(value["write_count"], 2)
            self.assertTrue(value["safe_return"])
            self.assertTrue(value["cleanup_started"])
            self.assertNotEqual(value["worker_parent_pid"], value["launcher_pid"])


def synthetic_subprocess_mode():
    folder = Path(os.sys.argv[2])
    if os.sys.argv[1] == "--synthetic-parent":
        subprocess.Popen([os.sys.executable, __file__, "--synthetic-worker", str(folder), str(os.getpid())],
                         env=os.environ.copy(), stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        os._exit(0)
    time.sleep(0.1)
    with tempfile.TemporaryDirectory(prefix="worker-fixture-", dir=folder) as inner:
        fixture = Fixture(inner, "reject_provision")
        data, external = fixture.execute()
        value = {"write_count": external.write_calls,
                 "safe_return": data["safe_return"], "cleanup_started": data["cleanup_started"],
                 "worker_parent_pid": os.getppid(), "launcher_pid": int(os.sys.argv[3])}
        old.write_private(folder / "result.json", json.dumps(value).encode("ascii"))


if __name__ == "__main__":
    if len(os.sys.argv) > 1 and os.sys.argv[1].startswith("--synthetic-"):
        synthetic_subprocess_mode()
    else:
        unittest.main()
