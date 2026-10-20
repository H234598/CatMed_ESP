#!/usr/bin/env python3
"""Check the portable IDF 6.1 SDK boundary without touching an SDK."""

import importlib.util
import os
from pathlib import Path
import tempfile
from unittest.mock import patch


TOOL = Path(__file__).resolve().parents[1] / "tools/prepare_iot_bridge_compat.py"
SDK_NAME = "esp-idf-v6.1-acme-20261019"


def load_tool():
    spec = importlib.util.spec_from_file_location("iot_bridge_compat_scope", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rejects(module, path: Path, ci_value: str) -> None:
    try:
        module._validate_idf_scope(path, ci_value)
    except ValueError:
        return
    raise AssertionError("Unapproved SDK path was accepted")


def main() -> None:
    with patch.dict(os.environ, {}, clear=True):
        default_tool = load_tool()
        default_sdk = Path.home() / ".local/share/katmed-tools" / SDK_NAME
        assert default_tool.IDF6_PATH == default_sdk
        assert default_tool._validate_idf_scope(default_sdk, "false") == default_sdk.resolve()
        rejects(default_tool, default_sdk.parent / "esp-idf-v5.5.5", "false")
        rejects(default_tool, Path("/opt/esp/idf"), "false")
        assert default_tool._validate_idf_scope(Path("/opt/esp/idf"), "true") == Path("/opt/esp/idf")
        rejects(default_tool, Path("/opt/esp/other"), "true")

    with tempfile.TemporaryDirectory(prefix="katmed-idf6-scope-") as scratch:
        with patch.dict(os.environ, {"KATMED_TOOLS_DIR": scratch}, clear=True):
            custom_tool = load_tool()
            custom_sdk = Path(scratch) / SDK_NAME
            assert custom_tool.IDF6_PATH == custom_sdk
            assert custom_tool._validate_idf_scope(custom_sdk, "false") == custom_sdk.resolve()
            rejects(custom_tool, default_sdk, "false")

    print("IDF 6.1 scope: portable local default, explicit tools root and CI boundary passed")


if __name__ == "__main__":
    main()
