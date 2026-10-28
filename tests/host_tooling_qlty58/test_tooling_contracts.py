#!/usr/bin/env python3
"""Real tooling functions with synthetic files and fake SDK commands only."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sizes = load_tool("acme_pair_sizes")
profile = load_tool("private_wifi_defaults")


class PairCacheContract(unittest.TestCase):
    def describe(self, cache):
        with tempfile.TemporaryDirectory(prefix="synthetic-size-contract-") as directory:
            root = Path(directory) / "build with spaces"
            root.mkdir()
            (root / "CMakeCache.txt").write_text(cache)
            for ext in ("bin", "elf", "map"):
                (root / ("katmed_foundation." + ext)).write_bytes(b"synthetic-not-firmware")

            def fake_command(argv, **kwargs):
                self.assertIsInstance(argv, list)
                self.assertTrue(kwargs["check"])
                self.assertEqual(Path(argv[-1]).parent, root)
                if "esp_idf_size" in argv:
                    return subprocess.CompletedProcess(argv, 0, json.dumps({
                        "layout": [{"name": "DRAM", "used": 17, "parts": []}]
                    }), "")
                self.assertEqual(argv[:2], ["riscv32-esp-elf-nm", "-C"])
                return subprocess.CompletedProcess(argv, 0, "", "")

            with mock.patch.object(sizes.subprocess, "run", side_effect=fake_command) as command:
                try:
                    result = sizes.describe(root, "c3", "baseline")
                except ValueError:
                    self.assertEqual(command.call_count, 0, "invalid cache must fail before SDK commands")
                    raise
            return result

    def test_reject_comment_shadow_wrong_target(self):
        cache = ("KATMED_ACME_RESOURCE_PROBE:BOOL=ON\n"
                 "KATMED_ACME_RESOURCE_CLIENT:BOOL=OFF\n"
                 "// Previous target IDF_TARGET:STRING=esp32c3\n"
                 "IDF_TARGET:STRING=esp32c6\n")
        with self.assertRaises(ValueError):
            self.describe(cache)

    def test_reject_prefix_values_and_duplicate_contracts(self):
        good = ("KATMED_ACME_RESOURCE_PROBE:BOOL=ON\n"
                "KATMED_ACME_RESOURCE_CLIENT:BOOL=OFF\n"
                "IDF_TARGET:STRING=esp32c3\n")
        for cache in [good.replace("=ON\n", "=ON_BUT_NOT_ON\n"),
                      good + "IDF_TARGET:STRING=esp32c6\n",
                      good + "IDF_TARGET:STRING=esp32c3\n"]:
            with self.subTest(cache_case="synthetic-invalid-contract"):
                with self.assertRaises(ValueError):
                    self.describe(cache)

    def test_valid_exact_cache_and_argv_spaces(self):
        result = self.describe("// Not the target: IDF_TARGET:STRING=esp32c6\n"
                               "KATMED_ACME_RESOURCE_PROBE:BOOL=ON\n"
                               "KATMED_ACME_RESOURCE_CLIENT:BOOL=OFF\n"
                               "IDF_TARGET:STRING=esp32c3\n")
        self.assertFalse(result["real_client_symbols"])
        self.assertEqual(result["static_memory_bytes"]["DRAM"]["used"], 17)


class ProfileValidationBeforePublication(unittest.TestCase):
    def config(self):
        return {"schema_version": 1, "provenance": "synthetic contract test only",
                "wifi": {"ssid": "synthetic-no-device", "password": "synthetic-not-a-credential"},
                "display": {"medication_alarm_text": "synthetic alarm", "seltsam_morse_text": "synthetic text"},
                "legacy_personal_documentation_text": "synthetic documentation never compiled"}

    def test_invalid_ui_preserves_both_existing_headers(self):
        for field in ("medication_alarm_text", "legacy_personal_documentation_text"):
            cfg = self.config()
            if field in cfg["display"]:
                cfg["display"][field] = ""
            else:
                cfg[field] = ""
            with self.subTest(field=field), tempfile.TemporaryDirectory(prefix="synthetic-profile-contract-") as directory:
                output = Path(directory) / "generated" / "wifi.h"
                output.parent.mkdir(mode=0o700)
                ui = output.with_name("ui.h")
                output.write_bytes(b"synthetic original wifi header")
                ui.write_bytes(b"synthetic original ui header")
                output.chmod(0o600)
                ui.chmod(0o600)
                before = output.read_bytes(), ui.read_bytes()
                with mock.patch.object(profile, "_private_regular_file", return_value=json.dumps(cfg).encode()):
                    with self.assertRaises(profile.ConfigError):
                        profile.generate(Path("synthetic-unopened-input"), output, ui)
                self.assertEqual((output.read_bytes(), ui.read_bytes()), before)

    def test_valid_pair_and_neutral_pair_stay_protected(self):
        for cfg in (self.config(), None):
            with self.subTest(neutral=cfg is None), tempfile.TemporaryDirectory(prefix="synthetic-profile-contract-") as directory:
                output = Path(directory) / "generated" / "wifi.h"
                ui = output.with_name("ui.h")
                with mock.patch.object(profile, "_private_regular_file", return_value=json.dumps(cfg).encode()) as reader:
                    profile.generate(None if cfg is None else Path("synthetic-unopened-input"), output, ui)
                self.assertEqual(reader.call_count, int(cfg is not None))
                self.assertIn(b"KATMED_WIFI_DEFAULT_SSID", output.read_bytes())
                self.assertIn(b"KATMED_PRIVATE_UI_PROFILE_H", ui.read_bytes())
                for path in (output, ui):
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_wifi_only_keeps_existing_optional_ui_contract(self):
        cfg = self.config()
        cfg["display"]["medication_alarm_text"] = ""
        with tempfile.TemporaryDirectory(prefix="synthetic-profile-contract-") as directory:
            output = Path(directory) / "generated" / "wifi.h"
            with mock.patch.object(profile, "_private_regular_file", return_value=json.dumps(cfg).encode()):
                profile.generate(Path("synthetic-unopened-input"), output)
            self.assertTrue(output.is_file())


class ExistingFailureContracts(unittest.TestCase):
    def test_longrun_interrupt_is_failure_and_never_completion(self):
        load_tool("katmed_usb_setup")
        longrun = load_tool("katmed_longrun")
        with tempfile.TemporaryDirectory(prefix="synthetic-longrun-contract-") as directory:
            root = Path(directory)
            args = argparse.Namespace(port="synthetic-no-port", expect_device="001122334455",
                                      expect_firmware="abcdef0", expect_build="abcdef012",
                                      duration_seconds=30, evidence_dir=str(root / "run"))
            messages = []
            def interrupted_opener(unused):
                raise KeyboardInterrupt()
            with self.assertRaises(longrun.LongrunError):
                longrun.run_longrun(args, opener=interrupted_opener,
                                    startup=lambda unused: None, emit=messages.append)
            self.assertFalse(messages)
            summary = json.loads((root / "run" / "summary.json").read_text())
            self.assertNotEqual(summary.get("completion_qualification"), "complete")

    def test_all_three_store_codec_build_failures_propagate(self):
        for name in ("katmed_store_v5_evidence", "katmed_store_v8_evidence", "katmed_store_v10_evidence"):
            with self.subTest(tool=name):
                tool = load_tool(name)
                with mock.patch.object(tool.subprocess, "run", return_value=subprocess.CompletedProcess([], 7)) as command:
                    with self.assertRaises(tool.Binding):
                        tool.decode(b"synthetic-not-a-store-record", ROOT)
                self.assertEqual(command.call_count, 1)
                self.assertIsInstance(command.call_args.args[0], list)



class ColorProfileArgumentContract(unittest.TestCase):
    def run_script(self, root, compiler_exit=0):
        script = root / "tests/host_c3_display_touch/check_color_profile.sh"
        script.parent.mkdir(parents=True)
        script.write_bytes((ROOT / "tests/host_c3_display_touch/check_color_profile.sh").read_bytes())
        fake_bin = root / "fake-bin"
        fake_bin.mkdir()
        log = root / "compiler-argv.jsonl"
        compiler = fake_bin / "gcc"
        compiler.write_text("#!" + sys.executable + "\n" + r'''
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ["SYNTHETIC_ARGV_LOG"], "a") as stream:
    stream.write(json.dumps(args) + "\n")
expected = '-DLV_CONF_PATH="' + os.environ["SYNTHETIC_REPO"] + '/firmware/components/katmed_images/host/lv_conf.h"'
if expected not in args:
    sys.exit(42)
if os.environ["SYNTHETIC_COMPILER_EXIT"] != "0":
    sys.exit(int(os.environ["SYNTHETIC_COMPILER_EXIT"]))
out = pathlib.Path(args[args.index("-o") + 1])
out.write_text("#!/bin/sh\nexit 0\n")
out.chmod(0o700)
''')
        compiler.chmod(0o700)
        env = dict(os.environ, PATH=str(fake_bin) + ":/usr/bin:/bin",
                   SYNTHETIC_REPO=str(root), SYNTHETIC_ARGV_LOG=str(log),
                   SYNTHETIC_COMPILER_EXIT=str(compiler_exit))
        result = subprocess.run(["/bin/bash", str(script)], env=env, capture_output=True, text=True)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        return result, calls

    def test_plain_and_space_paths_preserve_both_target_argument_vectors(self):
        with tempfile.TemporaryDirectory(prefix="synthetic-color-contract-") as directory:
            for label in ("plain", "path with spaces"):
                with self.subTest(checkout=label):
                    root = Path(directory) / label
                    result, calls = self.run_script(root)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(len(calls), 2)
                    for target, args in zip((3, 6), calls):
                        self.assertIn(f"-DCONFIG_IDF_TARGET_ESP32C{target}=1", args)
                        self.assertEqual([arg for arg in args if arg.startswith("-DLV_CONF_PATH=")],
                                         [f'-DLV_CONF_PATH="{root}/firmware/components/katmed_images/host/lv_conf.h"'])
                        self.assertEqual(args[args.index("-o") + 1],
                                         str(root / f"build-ui-host/c3-repro/color_profile_c{target}"))

    def test_compiler_failure_stops_before_second_target(self):
        with tempfile.TemporaryDirectory(prefix="synthetic-color-failure-") as directory:
            result, calls = self.run_script(Path(directory) / "plain", compiler_exit=37)
            self.assertEqual(result.returncode, 37)
            self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()
