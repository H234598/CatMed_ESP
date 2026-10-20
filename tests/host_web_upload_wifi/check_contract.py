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
RESET_C = ROOT / "firmware/components/katmed_web/katmed_web_reset.c"
HTML = ROOT / "firmware/components/katmed_web/ui/index.html"
CSS = ROOT / "firmware/components/katmed_web/ui/style.css"
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
        self.assertIn("katmed_image_store_open_read", source)
        self.assertIn("katmed_image_store_read", source)
        self.assertIn("katmed_image_store_import_catalog", source)
        self.assertIn('"/api/catalog/images"', source)
        self.assertNotIn("fopen(", source)

    def test_wifi_profiles_are_bounded_atomic_and_secrets_are_not_returned(self):
        source = WIFI_C.read_text(encoding="utf-8")
        header = (ROOT / "firmware/components/katmed_web/include/katmed_web_wifi.h").read_text(encoding="utf-8")
        self.assertIn("profile_limit", header)
        self.assertIn("save_profiles_atomic", header)
        self.assertIn("KATMED_WEB_WIFI_CREDENTIAL_KEEP", header)
        self.assertIn("KATMED_WEB_WIFI_CREDENTIAL_REPLACE", header)
        self.assertIn("KATMED_WEB_WIFI_CREDENTIAL_OPEN", header)
        self.assertIn('"/api/wifi/profiles"', source)
        self.assertIn("profile_limit", source)
        self.assertIn("save_profiles_atomic", source)
        self.assertIn("authorize(req, true)", source)
        self.assertIn('"/api/wifi/reconnect"', source)
        self.assertIn("reevaluate_priority", source)
        self.assertIn("legacy", header.lower())
        self.assertNotIn("httpd_resp_send(req, password", source)
        self.assertNotIn("ESP_LOG", source)
        # Credentials are parsed only for the atomic save callback, never copied
        # into the metadata GET representation.
        get_source = source[source.index("static esp_err_t wifi_get"):source.index("static bool read_body")]
        self.assertNotIn("password", get_source)
        self.assertIn("priority", get_source)

    def test_reset_has_no_http_confirmation_or_token(self):
        source = RESET_C.read_text(encoding="utf-8")
        self.assertIn('"/api/factory-reset/prepare"', source)
        self.assertIn('"/api/factory-reset/status"', source)
        self.assertIn('"/api/factory-reset/cancel"', source)
        self.assertNotIn('"/api/factory-reset/confirm"', source)
        self.assertIn("authorize(req, true)", source)
        self.assertIn("authorize(req, false)", source)
        self.assertNotIn("confirmation", source)
        self.assertIn("Cache-Control", source)

    def test_advanced_and_reset_warning_layout_contract(self):
        html = HTML.read_text(encoding="utf-8")
        main = html[html.index('id="main-settings"'):html.index('id="advanced-view"')]
        self.assertIn('id="open-advanced"', main)
        self.assertNotIn('id="medication-form"', main)
        self.assertNotIn('id="image-form"', main)
        self.assertNotIn('id="due-form"', main)
        self.assertNotIn('id="status-button"', html)
        self.assertNotIn('id="back-button"', html)
        self.assertIn('id="factory-reset-no" class="secondary reset-no" type="button" autofocus', html)
        self.assertIn('id="factory-reset-status"', html)
        self.assertNotIn('id="factory-reset-yes"', html)
        self.assertIn("Mesh-Bindungen", html)
        self.assertIn("privaten Build", html)
        self.assertIn("einkompilierte private Vorgaben", html)
        self.assertIn("einkompilierte private Vorgaben erhalten", (ROOT / "firmware/components/katmed_ui/ui.c").read_text(encoding="utf-8"))
        css = CSS.read_text(encoding="utf-8")
        self.assertIn(".reset-no{width:100%;height:72px", css)
        ui = (ROOT / "firmware/components/katmed_ui/ui.c").read_text(encoding="utf-8")
        self.assertIn("lv_obj_set_flex_grow(warning_view, 1)", ui)
        self.assertIn("lv_obj_set_size(yes, yes_width, yes_height)", ui)
        self.assertIn("background:#ffec73", css)
        self.assertIn("dialog.showModal()", APP_JS.read_text(encoding="utf-8"))
        self.assertIn('$("factory-reset-no").focus()', APP_JS.read_text(encoding="utf-8"))

    def test_ui_wires_upload_wifi_and_csrf(self):
        source = APP_JS.read_text(encoding="utf-8")
        self.assertIn('api("/api/images", "POST", file)', source)
        self.assertIn('"/api/catalog/images?offset="', source)
        self.assertIn('api("/api/images/import" + query, "POST")', source)
        self.assertIn('"/api/images?id=" + encodeURIComponent(image.id)', source)
        self.assertIn('api("/api/wifi/profiles", "PUT", {profiles})', source)
        self.assertIn('"/api/wifi/reconnect", "POST"', source)
        self.assertIn('api("/api/factory-reset/prepare", "POST", {})', source)
        self.assertIn('api("/api/factory-reset/cancel", "POST", {})', source)
        self.assertNotIn('"/api/factory-reset/confirm"', source)
        self.assertIn('headers["X-Katmed-CSRF"]', source)
        self.assertIn('credentials: "same-origin"', source)
        self.assertIn("window.scrollTo(0, state.advancedScrollY)", source)
        self.assertIn("dx > 64 && Math.abs(dx) > Math.abs(dy) * 1.4", source)
        self.assertIn("priority - b.profile.priority || a.index - b.index", source)


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
