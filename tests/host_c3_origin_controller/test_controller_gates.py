"""Portfreie Gegenproben für den echten privaten C3-Ablaufcontroller.

Der Controller wird bis in seine Hauptfolge ausgeführt. Sämtliche externen
Prozesse werden davor durch einen lokalen Fake ersetzt; nur synthetische
Dateien unter TemporaryDirectory werden geschrieben. Der Pfad der zu prüfenden
privaten Controllerkopie kommt ausschließlich aus C3_CONTROLLER_UNDER_TEST.
"""

import ast
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess as real_subprocess
import sys
import tempfile
import time
import types
import unittest


IMAGE_SIZE = 4 * 1024 * 1024
NEW_BLOCK_SHA = "f3b6fe4a23944e6a3b6cb6b46e8463d5aa93bc1df53b9b2333fbcd38a7035626"
NEW_BLOCK_NAME = "c3_origin_label_diagnose_inline.py"
OLD_BLOCK_NAME = "c3_phase_rejection_diagnose_inline.py"
HOST_HEAD = "9ef2e8d1f7bea6ca98b68e5bf3f18d701d6ffb25"
OLD_HOST_HEAD = "97c2eb3d94aa2753982324aa6ffa513ef7e992a0"
FIRM_HEAD = "224e8035bb0e5bc5f865aed019ad8c2e5673cd43"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_private(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
    assert stat.S_IMODE(path.lstat().st_mode) == 0o600


class Clock:
    def __init__(self):
        self.seconds = 0

    def monotonic_ns(self):
        return self.seconds * 1_000_000_000


class FakeExternal:
    """Strict fake: unknown external commands are test failures."""

    DEVNULL = real_subprocess.DEVNULL

    def __init__(self, fixture, scenario, old_controller):
        self.fixture = fixture
        self.scenario = scenario
        self.old_controller = old_controller
        self.calls = []
        self.id_calls = 0
        self.fuser_calls = 0
        self.write_calls = 0
        self.read_calls = 0
        self.recorder_calls = 0
        self.pid = 40000

    def run(self, argv, **_kwargs):
        argv = [str(value) for value in argv]
        self.calls.append(("run", tuple(argv)))
        if argv[0] == "git":
            if "rev-parse" in argv:
                head = FIRM_HEAD if "firmware" in argv[2] or "km1_phasen" in argv[2] else (
                    OLD_HOST_HEAD if self.old_controller else HOST_HEAD
                )
                if self.scenario == "wrong_head" and head != FIRM_HEAD:
                    head = "0" * 40
                if self.scenario == "wrong_firmware_head" and head == FIRM_HEAD:
                    head = "0" * 40
                return types.SimpleNamespace(returncode=0, stdout=head + "\n", stderr="")
            if "status" in argv:
                return types.SimpleNamespace(returncode=0, stdout="", stderr="")
        if argv[0] == "fuser":
            self.fuser_calls += 1
            busy = self.scenario == "final_busy" and self.fuser_calls >= 4
            return types.SimpleNamespace(returncode=0 if busy else 1, stdout=b"", stderr=b"")
        if argv[0] == "udevadm":
            props = (
                "ID_VENDOR_ID=303a\nID_MODEL_ID=1001\n"
                "ID_SERIAL_SHORT=AC:27:6E:46:BC:2C\nID_USB_INTERFACE_NUM=00\n"
            )
            return types.SimpleNamespace(returncode=0, stdout=props, stderr="")
        if "read-mac" in argv or "flash-id" in argv:
            self.id_calls += 1
            good = (
                "ESP32-C3 revision v0.4 ac:27:6e:46:bc:2c "
                "Embedded Flash 4MB (XMC) Detected flash size: 4MB"
            )
            if self.scenario == "wrong_identity" or (
                self.scenario == "final_identity" and self.id_calls >= 5
            ):
                good = "synthetic wrong identity"
            return types.SimpleNamespace(returncode=0, stdout=good, stderr="")
        raise AssertionError("Unexpected external run command")

    def Popen(self, argv, **kwargs):
        argv = [str(value) for value in argv]
        self.pid += 1
        if "write-flash" in argv:
            kind = "write"
        elif "read-flash" in argv:
            kind = "read"
        elif argv[-1].endswith(".py"):
            kind = "recorder"
        else:
            raise AssertionError("Unexpected external Popen command")
        self.calls.append((kind, tuple(argv)))
        owner = self
        pid = self.pid

        class Process:
            def __init__(self):
                self.pid = pid

            def wait(self):
                owner.fixture.clock.seconds += 1
                if kind == "write":
                    owner.write_calls += 1
                    owner.fixture.flash = (
                        owner.fixture.product if owner.write_calls == 1 else owner.fixture.factory
                    )
                    return 0
                if kind == "read":
                    owner.read_calls += 1
                    output = Path(argv[-1])
                    data = owner.fixture.flash
                    if owner.scenario == "bad_product_readback" and owner.read_calls == 1:
                        data = bytes([data[0] ^ 1]) + data[1:]
                    if owner.scenario in {"bad_final_readback", "hash_only_final_mismatch"} and owner.read_calls == 3:
                        data = bytes([data[0] ^ 1]) + data[1:]
                    write_private(output, data)
                    if owner.scenario == "deadline_after_write" and owner.read_calls == 1:
                        owner.fixture.clock.seconds = 800
                    return 0
                owner.recorder_calls += 1
                evidence = owner.fixture.run_dir / "recorder"
                evidence.mkdir(mode=0o700, exist_ok=True)
                write_private(evidence / "diagnosis.json", b'{"first_error":"other_safe_error"}')
                return 1 if owner.scenario == "recorder_error" else 0

        return Process()


class ControllerFixture:
    def __init__(self, root, controller, scenario):
        self.root = Path(root)
        self.controller = Path(controller)
        self.scenario = scenario
        self.old_controller = "c3-phase-rejection-" in str(self.controller)
        self.run_dir = self.root / "run"
        self.run_dir.mkdir(mode=0o700)
        self.host = self.root / "host"
        self.firm = self.root / "firmware"
        self.host.mkdir()
        self.firm.mkdir()
        self.clock = Clock()
        self.factory = bytes(IMAGE_SIZE)
        self.flash = self.factory
        self.parts = []
        image = bytearray(self.factory)
        for index, offset in enumerate((0, 0x8000, 0x10000, 0x2C0000)):
            data = ("SYNTHETIC_PART_" + str(index)).encode("ascii")
            part = self.root / ("part" + str(index) + ".bin")
            write_private(part, data)
            self.parts.append((offset, part, len(data), digest(data)))
            image[offset : offset + len(data)] = data
        self.product = bytes(image)
        self.factory_path = self.root / "factory.bin"
        write_private(self.factory_path, self.factory)
        self.prewrite = self.run_dir / "prewrite-fullflash.bin"
        if scenario != "missing_prewrite":
            prewrite = self.factory
            if scenario == "different_prewrite":
                prewrite = bytes([1]) + prewrite[1:]
            write_private(self.prewrite, prewrite)
            if scenario == "loose_prewrite":
                self.prewrite.chmod(0o644)
            if scenario == "symlink_prewrite":
                self.prewrite.unlink()
                self.prewrite.symlink_to(self.factory_path)
        block = Path(os.environ["C3_NEW_BLOCK_UNDER_TEST"]).read_bytes()
        assert len(block) == 9457 and digest(block) == NEW_BLOCK_SHA
        write_private(self.run_dir / NEW_BLOCK_NAME, block)
        write_private(self.run_dir / OLD_BLOCK_NAME, block)
        if scenario == "wrong_template":
            template = self.run_dir / NEW_BLOCK_NAME
            template.write_bytes(b"SYNTHETIC_WRONG_TEMPLATE")
        self.port = self.root / "synthetic-by-id"
        self.port.symlink_to("/dev/null")
        self.external = FakeExternal(self, scenario, self.old_controller)
        self.source_fixtures = {}
        for relative, expected in (
            ("tools/katmed_usb_setup.py", "a86e746a140d14592ac026ca5c5e055abda7fc78b816c38bdc25df85155b499a"),
            ("tools/katmed_longrun.py", "c982c5e116cb2f0a76386f9c89a9ec36ab219d8bbe2d9e36e4cdd0636be7eff2"),
            ("firmware/build-c3-km1-phasen-final/katmed_foundation.elf", "29a1f0235fd18ca86196a53163554250250a7b1c6d9891d1c7f22dd28788b161"),
            ("firmware/build-c3-km1-phasen-final/project_description.json", "314691a1892ad2a17e18a19029c3dbdb937626fc2223027a1f0973d1875df9e4"),
        ):
            path = (self.host if relative.startswith("tools/") else self.firm) / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            data = b'{"target":"esp32c3","project_version":"224e803"}' if path.name.endswith(".json") else relative.encode()
            write_private(path, data)
            self.source_fixtures[path] = (data, expected)
        if scenario == "wrong_source":
            source = self.host / "tools/katmed_longrun.py"
            source.write_bytes(b"DIFFERENT_SYNTHETIC_SOURCE")

    def execute(self):
        tree = ast.parse(self.controller.read_text(), filename=str(self.controller))
        assert isinstance(tree.body[-1], ast.Try)
        definitions = ast.Module(
            body=[
                node for node in tree.body[:-1]
                if not (
                    isinstance(node, ast.Assign)
                    and any(isinstance(target, ast.Name) and target.id == "START" for target in node.targets)
                )
            ],
            type_ignores=[],
        )
        action = ast.Module(body=[tree.body[-1]], type_ignores=[])
        scope = {
            "__name__": "synthetic_controller",
            "START": {"port": str(self.port), "t0_monotonic_ns": 0},
        }
        exec(compile(definitions, str(self.controller), "exec"), scope)
        scope.update(
            RUN=self.run_dir,
            HOST=self.host,
            FIRM=self.firm,
            FACTORY=self.factory_path,
            EXPECTED_FACTORY=digest(self.factory),
            BINS=self.parts,
            PORT=str(self.port),
            ESP="/synthetic/no-esptool-executable",
            time=types.SimpleNamespace(monotonic_ns=self.clock.monotonic_ns),
            subprocess=self.external,
        )
        scope["START"]["t0_monotonic_ns"] = 0
        original_sha = scope["sha"]

        def synthetic_source_sha(path):
            path = Path(path)
            if self.scenario == "hash_only_final_mismatch" and path.name == "restored-fullflash.bin":
                return digest(self.factory)
            if path in self.source_fixtures:
                data, expected = self.source_fixtures[path]
                if path.read_bytes() == data:
                    return expected
            return original_sha(path)

        scope["sha"] = synthetic_source_sha
        env_before = os.environ.copy()
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                exec(compile(action, str(self.controller), "exec"), scope)
        finally:
            os.environ.clear()
            os.environ.update(env_before)
        return scope["D"], self.external


class ControllerGates(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.controller = Path(os.environ["C3_CONTROLLER_UNDER_TEST"])
        assert cls.controller.is_file() and not cls.controller.is_symlink()

    def run_case(self, scenario="normal"):
        with tempfile.TemporaryDirectory(prefix="c3-controller-synthetic-") as folder:
            fixture = ControllerFixture(folder, self.controller, scenario)
            return fixture.execute()

    def test_01_new_template_binding_reaches_one_recording(self):
        data, fake = self.run_case()
        self.assertEqual(fake.recorder_calls, 1)
        self.assertEqual(fake.write_calls, 2)
        self.assertEqual(fake.read_calls, 3)
        self.assertTrue(data["safe_return"])
        self.assertIsNone(data["first_error"])
        names = [item[0] for item in fake.calls if item[0] != "run"]
        self.assertEqual(names, ["write", "read", "recorder", "read", "write", "read"])
        offset_steps = [step for step in data["steps"] if "offset" in step]
        self.assertEqual(len(offset_steps), 4)

    def test_02_prewrite_and_source_fail_before_mutation(self):
        for scenario in (
            "wrong_head", "wrong_firmware_head", "wrong_source", "wrong_template",
            "wrong_identity", "missing_prewrite",
            "different_prewrite", "loose_prewrite", "symlink_prewrite",
        ):
            with self.subTest(scenario=scenario):
                data, fake = self.run_case(scenario)
                self.assertEqual(fake.write_calls, 0)
                self.assertEqual(fake.recorder_calls, 0)
                self.assertFalse(data["safe_return"])
                self.assertFalse(data["write_started"])

    def test_03_postwrite_errors_restore_without_second_recording(self):
        for scenario in ("bad_product_readback", "recorder_error", "deadline_after_write"):
            with self.subTest(scenario=scenario):
                data, fake = self.run_case(scenario)
                self.assertEqual(fake.write_calls, 2)
                self.assertTrue(data["cleanup_started"])
                self.assertLessEqual(fake.recorder_calls, 1)
                self.assertIsNotNone(data["first_error"])

    def test_04_final_proof_or_identity_missing_is_not_safe(self):
        for scenario in (
            "bad_final_readback", "hash_only_final_mismatch", "final_identity", "final_busy"
        ):
            with self.subTest(scenario=scenario):
                data, fake = self.run_case(scenario)
                self.assertEqual(fake.write_calls, 2)
                self.assertFalse(data["safe_return"])
                self.assertIsNotNone(data["first_error"])

    def test_05_no_external_process_escapes_fake(self):
        data, fake = self.run_case()
        self.assertTrue(data["safe_return"])
        self.assertTrue(all(kind in {"run", "write", "read", "recorder"} for kind, _ in fake.calls))
        self.assertFalse(any(argv and argv[0] in {"kill", "pkill", "killall"} for _, argv in fake.calls))

    def test_06_detached_worker_restores_after_parent_exit(self):
        with tempfile.TemporaryDirectory(prefix="c3-controller-orphan-synthetic-") as folder:
            outcome = Path(folder) / "orphan-outcome.json"
            launcher = real_subprocess.run(
                [sys.executable, __file__, "--synthetic-parent", folder, str(self.controller)],
                env=os.environ.copy(),
                capture_output=True,
                timeout=5,
            )
            self.assertEqual(launcher.returncode, 0)
            deadline = time.monotonic() + 5
            while not outcome.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(outcome.exists())
            result = json.loads(outcome.read_text())
            self.assertEqual(result["write_count"], 2)
            self.assertEqual(result["recorder_count"], 1)
            self.assertTrue(result["cleanup_started"])
            self.assertTrue(result["safe_return"])
            self.assertEqual(result["first_error"], "recorder_nonzero")
            self.assertNotEqual(result["worker_parent_pid"], result["launcher_pid"])


def synthetic_subprocess_mode():
    mode = sys.argv[1]
    root, controller = sys.argv[2:4]
    if mode == "--synthetic-parent":
        real_subprocess.Popen(
            [sys.executable, __file__, "--synthetic-worker", root, controller, str(os.getpid())],
            env=os.environ.copy(),
            stdin=real_subprocess.DEVNULL,
            stdout=real_subprocess.DEVNULL,
            stderr=real_subprocess.DEVNULL,
            start_new_session=True,
        )
        os._exit(0)
    time.sleep(0.1)
    with tempfile.TemporaryDirectory(prefix="worker-fixture-", dir=root) as fixture_root:
        fixture = ControllerFixture(fixture_root, controller, "recorder_error")
        data, fake = fixture.execute()
        result = {
            "write_count": fake.write_calls,
            "recorder_count": fake.recorder_calls,
            "cleanup_started": data["cleanup_started"],
            "safe_return": data["safe_return"],
            "first_error": data["first_error"],
            "worker_parent_pid": os.getppid(),
            "launcher_pid": int(sys.argv[4]),
        }
        write_private(Path(root) / "orphan-outcome.json", json.dumps(result).encode())


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1].startswith("--synthetic-"):
        synthetic_subprocess_mode()
    else:
        unittest.main()
