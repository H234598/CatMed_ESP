#!/usr/bin/env python3
"""Bounded host checks for web upload/WLAN contracts and gift privacy gate."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MEDIA_C = ROOT / "firmware/components/katmed_web/katmed_web_media.c"
WIFI_C = ROOT / "firmware/components/katmed_web/katmed_web_wifi.c"
APP_JS = ROOT / "firmware/components/katmed_web/ui/app.js"
CHECKER = ROOT / "tools/check_gift_build_privacy.py"


class RouteContractTests(unittest.TestCase):
    def test_media_upload_is_bounded_streamed_and_aborts_failures(self):
        source = MEDIA_C.read_text(encoding="utf-8")
        self.assertIn("KATMED_IMAGE_UPLOAD_MAX_BYTES", source)
        self.assertIn("httpd_req_recv", source)
        self.assertIn("katmed_image_store_upload_begin", source)
        self.assertIn("katmed_image_store_upload_write", source)
        self.assertIn("katmed_image_store_upload_commit", source)
        self.assertGreaterEqual(source.count("katmed_image_store_upload_abort"), 2)
        self.assertIn('strcmp(content_type, "image/jpeg")', source)
        self.assertIn("authorize(req, true)", source)
        self.assertNotIn("req->user_ctx", source)

    def test_metadata_routes_are_authenticated_and_id_only(self):
        source = MEDIA_C.read_text(encoding="utf-8")
        self.assertIn("authorize(req, false)", source)
        self.assertIn("authorize(req, true)", source)
        self.assertIn("katmed_image_store_list", source)
        self.assertIn("katmed_image_store_remove(id)", source)
        self.assertNotIn("open_read", source)
        self.assertNotIn("fopen(", source)

    def test_wifi_saves_before_separate_reconnect_and_never_returns_credentials(self):
        source = WIFI_C.read_text(encoding="utf-8")
        self.assertIn("save_durable", source)
        self.assertIn("schedule_reconnect", source)
        self.assertIn('"/api/wifi/reconnect"', source)
        self.assertIn("recovery_available", source)
        self.assertIn("authorize(req, true)", source)
        self.assertNotIn("httpd_resp_send(req, ssid", source)
        self.assertNotIn("httpd_resp_send(req, password", source)
        self.assertNotIn("ESP_LOG", source)

    def test_ui_wires_upload_wifi_and_csrf(self):
        source = APP_JS.read_text(encoding="utf-8")
        self.assertIn('api("/api/images", "POST", file)', source)
        self.assertIn('"/api/wifi", "POST"', source)
        self.assertIn('"/api/wifi/reconnect", "POST"', source)
        self.assertIn('headers["X-Katmed-CSRF"]', source)
        self.assertIn('credentials: "same-origin"', source)


class GiftPrivacyCheckerTests(unittest.TestCase):
    def make_input(self, root: Path, value: str) -> Path:
        path = root / "local-private-input"
        path.write_text("private_wifi_password=" + value + "\n", encoding="utf-8")
        path.chmod(0o600)
        return path

    def make_build(self, root: Path, artifact: bytes = b"neutral firmware") -> Path:
        build = root / "build"
        build.mkdir()
        (build / "sdkconfig").write_text("CONFIG_KATMED_GIFT_BUILD=y\n", encoding="utf-8")
        (build / "compile_commands.json").write_text(
            '[{"command":"xtensa-esp32-elf-gcc -DKATMED_GIFT_BUILD=1"}]',
            encoding="utf-8")
        (build / "firmware.elf").write_bytes(artifact)
        return build

    def run_checker(self, build: Path, private_input: Path):
        env = os.environ.copy()
        env["KATMED_GIFT_PRIVACY_INPUT"] = str(private_input)
        return subprocess.run(
            [sys.executable, str(CHECKER), str(build)],
            cwd=ROOT, env=env, capture_output=True, text=True, check=False)

    def test_neutral_build_passes_without_exposing_private_value(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            value = "synthetic-private-value-only-for-test"
            private_input = self.make_input(root, value)
            result = self.run_checker(self.make_build(root), private_input)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("found=0", result.stdout)
            self.assertNotIn(value, result.stdout + result.stderr)
            self.assertNotIn(str(private_input), result.stdout + result.stderr)

    def test_private_value_fails_with_category_only(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            value = "synthetic-private-value-only-for-test"
            private_input = self.make_input(root, value)
            result = self.run_checker(self.make_build(root, value.encode()), private_input)
            self.assertEqual(result.returncode, 1)
            self.assertIn("private_wifi_password", result.stdout)
            self.assertNotIn(value, result.stdout + result.stderr)

    def test_missing_marker_and_unsafe_input_fail_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            private_input = self.make_input(root, "synthetic-value")
            build = self.make_build(root)
            (build / "sdkconfig").write_text("# CONFIG_KATMED_GIFT_BUILD is not set\n",
                                              encoding="utf-8")
            result = self.run_checker(build, private_input)
            self.assertEqual(result.returncode, 1)
            self.assertIn("marker missing", result.stdout)
            private_input.chmod(0o644)
            second = root / "second"
            second.mkdir()
            result = self.run_checker(self.make_build(second), private_input)
            self.assertEqual(result.returncode, 1)
            self.assertIn("permissions", result.stdout)


if __name__ == "__main__":
    unittest.main()
