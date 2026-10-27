"""Compile the production policy against a real resolved public SDK config.

CI dependencies generate firmware/build-c3/config/sdkconfig.h before Host.
Local public layout experiments may select their header through the environment.
This is a build-policy regression; it does not emulate radio traffic or claim
runtime heap/latency acceptance.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CONFIG = Path(os.environ.get("KATMED_C3_RESOURCE_CONFIG",
                            ROOT / "firmware/build-c3/config/sdkconfig.h"))


class C3ResourcePolicy(unittest.TestCase):
    def compile_policy(self, overrides=""):
        self.assertTrue(CONFIG.is_file(), "Resolved public C3 SDK header is mandatory")
        with tempfile.TemporaryDirectory(prefix="katmed-c3-policy-") as tmp:
            source = Path(tmp) / "policy.c"
            binary = Path(tmp) / "policy"
            source.write_text('#include "sdkconfig.h"\n' + overrides +
                              '\n#include "c3_resource_policy.h"\n'
                              'int main(void) { return 0; }\n')
            result = subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-DESP_PLATFORM=1", "-I" + str(CONFIG.parent),
                "-I" + str(ROOT / "firmware/main"),
                str(source), "-o", str(binary),
            ], capture_output=True, text=True)
            if result.returncode == 0:
                subprocess.run([str(binary)], check=True)
            return result

    def test_actual_resolved_public_config(self):
        result = self.compile_policy()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_retained_iram_choice_is_rejected(self):
        result = self.compile_policy("#undef CONFIG_ESP_WIFI_IRAM_OPT\n"
                                     "#define CONFIG_ESP_WIFI_IRAM_OPT 1\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("C3_RESOURCE_POLICY_PLACEMENT", result.stderr)

    def test_unmatched_rx_window_is_rejected(self):
        result = self.compile_policy("#undef CONFIG_ESP_WIFI_RX_BA_WIN\n"
                                     "#define CONFIG_ESP_WIFI_RX_BA_WIN 6\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("C3_RESOURCE_POLICY_RX", result.stderr)

    def test_ampdu_feature_cannot_be_removed_for_memory(self):
        result = self.compile_policy("#undef CONFIG_ESP_WIFI_AMPDU_RX_ENABLED\n"
                                     "#define CONFIG_ESP_WIFI_AMPDU_RX_ENABLED 0\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("C3_RESOURCE_POLICY_FEATURES", result.stderr)

    def test_dynamic_tx_cannot_be_replaced_by_static_pool(self):
        result = self.compile_policy("#undef CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER\n"
                                     "#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER 0\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("C3_RESOURCE_POLICY_DYNAMIC", result.stderr)

    def test_c6_does_not_inherit_the_c3_policy(self):
        result = self.compile_policy("#undef CONFIG_IDF_TARGET_ESP32C3\n"
                                     "#define CONFIG_IDF_TARGET_ESP32C3 0\n"
                                     "#undef CONFIG_LWIP_MAX_SOCKETS\n"
                                     "#define CONFIG_LWIP_MAX_SOCKETS 12\n"
                                     "#undef CONFIG_ESP_WIFI_IRAM_OPT\n"
                                     "#define CONFIG_ESP_WIFI_IRAM_OPT 1\n")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_stale_resolved_c3_socket_limit_is_rejected(self):
        result = self.compile_policy("#undef CONFIG_LWIP_MAX_SOCKETS\n"
                                     "#define CONFIG_LWIP_MAX_SOCKETS 10\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FULLSTACK_SOCKET_POLICY", result.stderr)

    def test_stale_resolved_c6_socket_limit_is_rejected(self):
        result = self.compile_policy("#undef CONFIG_IDF_TARGET_ESP32C3\n"
                                     "#define CONFIG_IDF_TARGET_ESP32C3 0\n"
                                     "#undef CONFIG_IDF_TARGET_ESP32C6\n"
                                     "#define CONFIG_IDF_TARGET_ESP32C6 1\n"
                                     "#undef CONFIG_LWIP_MAX_SOCKETS\n"
                                     "#define CONFIG_LWIP_MAX_SOCKETS 10\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FULLSTACK_SOCKET_POLICY", result.stderr)


if __name__ == "__main__":
    unittest.main()
