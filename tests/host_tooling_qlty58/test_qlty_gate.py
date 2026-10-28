#!/usr/bin/env python3
"""Exercise the real pinned CLI and production SARIF gate on synthetic sources."""
import importlib.util
import datetime
import time
import json
import os
from pathlib import Path
import subprocess  # nosec B404: required by the synthetic native-CLI contract test.
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_qlty", ROOT / "tools/check_qlty.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)
CONFIG = '''config_version = "0"
[[source]]
name = "default"
default = true
[[plugin]]
name = "ruff"
version = "0.14.6"
drivers = ["lint"]
'''
BROKEN = '''
[plugins.definitions.broken59]
file_types = ["python"]
latest_version = "1.0.0"
known_good_version = "1.0.0"
[plugins.definitions.broken59.drivers.lint]
script = "katmed-deliberately-missing-analyzer-59 ${target}"
success_codes = [0]
error_codes = [127]
output = "stdout"
output_format = "ruff"
suggested = "targets"
[[plugin]]
name = "broken59"
version = "1.0.0"
drivers = ["lint"]
'''


class NativeQltyGate(unittest.TestCase):
    def scan(self, root, config=CONFIG, source="import os\n"):
        root.mkdir()
        (root / ".qlty").mkdir()
        (root / ".qlty/qlty.toml").write_text(config)
        code = root / "synthetic.py"
        code.write_text(source)
        subprocess.run(["/usr/bin/git", "init", "--quiet"], cwd=root, check=True)  # nosec B603: fixed git argv, own synthetic tree.
        subprocess.run(["/usr/bin/git", "add", "--", "."], cwd=root, check=True)  # nosec B603: fixed git argv, own synthetic tree.
        start = datetime.datetime.now(datetime.timezone.utc).isoformat()
        mono = time.monotonic_ns()
        self.assertTrue(Path(os.environ["QLTY_BIN"]).is_absolute())
        child = subprocess.Popen([  # nosec B603: pinned Qlty CLI, synthetic source, no shell.
            os.environ["QLTY_BIN"], "check", "--all", "--no-fix", "--no-formatters",
            "--no-cache", "--no-progress", "--sarif", "--skip-source-fetch", "--no-upgrade-check",
        ], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, _ = child.communicate()
        print(json.dumps(dict(native_qlty_PID=child.pid, startUTC=start,
                              endUTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                              start_monotonic_ns=mono, end_monotonic_ns=time.monotonic_ns(),
                              Exit=child.returncode, ChildEnd=True,
                              streams_closed=child.stdout.closed and child.stderr.closed)))
        self.assertEqual(code.read_text(), source, "analysis must not modify source")
        report = root / "report.sarif"
        report.write_bytes(stdout)
        return report, child.returncode

    def test_real_native_failure_matrix_and_delta(self):
        with tempfile.TemporaryDirectory(prefix="synthetic-qlty-gate-") as temporary:
            root = Path(temporary)
            clean, clean_exit = self.scan(root / "clean", source="answer = 42\n")
            issue, issue_exit = self.scan(root / "issue")
            self.assertEqual(clean_exit, 0)
            self.assertEqual(issue_exit, 1)
            self.assertIn("ruff:F401", [r["ruleId"] for r in json.loads(issue.read_text())["runs"][0]["results"]])
            self.assertEqual(GATE.compare(clean, clean_exit, issue, issue_exit), 1)
            self.assertEqual(GATE.compare(issue, issue_exit, clean, clean_exit), 0)
            # The narrow inline contexts above do not disable Bandit elsewhere.
            bandit_config = CONFIG.replace('name = "ruff"', 'name = "bandit"').replace('version = "0.14.6"', 'version = "1.9.2"')
            security, security_exit = self.scan(root / "security", bandit_config, "import subprocess\n")
            self.assertEqual(security_exit, 1)
            self.assertIn("bandit:B404", [r["ruleId"] for r in json.loads(security.read_text())["runs"][0]["results"]])
            for name, config in (("operation", CONFIG + BROKEN), ("invalid", "config_version = [")):
                report, exit_code = self.scan(root / name, config)
                with self.subTest(case=name), self.assertRaises((GATE.IncompleteScan, ValueError)):
                    GATE.read_report(report, exit_code)
                if name == "operation":
                    native = json.loads(report.read_text())["runs"][0]
                    self.assertFalse(native["invocations"][0]["executionSuccessful"])
                    self.assertIn("ruff:F401", [r["ruleId"] for r in native["results"]])
            for name, content in (("missing", None), ("empty", ""), ("truncated", issue.read_text()[:40]),
                                  ("incomplete", '{"version":"2.1.0","runs":[]}')):
                path = root / name
                if content is not None:
                    path.write_text(content)
                with self.subTest(case=name), self.assertRaises((GATE.IncompleteScan, OSError, ValueError)):
                    GATE.read_report(path, 0)


if __name__ == "__main__":
    unittest.main()
