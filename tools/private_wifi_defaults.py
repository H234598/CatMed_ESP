#!/usr/bin/env python3
"""Validate the private build-time WLAN config and emit a protected C header."""

import argparse
import json
import os
import pathlib
import stat
import sys
import tempfile


class ConfigError(Exception):
    pass


def _private_regular_file(path: pathlib.Path) -> bytes:
    try:
        parent = path.parent.lstat()
        info = path.lstat()
    except OSError as exc:
        raise ConfigError("private WLAN config is unavailable") from exc
    if (not stat.S_ISDIR(parent.st_mode) or stat.S_ISLNK(parent.st_mode)
            or parent.st_uid != os.getuid() or stat.S_IMODE(parent.st_mode) != 0o700):
        raise ConfigError("private WLAN config directory must be owned and mode 0700")
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or stat.S_IMODE(info.st_mode) != 0o600):
        raise ConfigError("private WLAN config must be an owned regular mode-0600 file")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise ConfigError("private WLAN config cannot be read") from exc


def _value(value: object, name: str, minimum: int, maximum: int) -> bytes:
    if not isinstance(value, str):
        raise ConfigError(f"private WLAN {name} must be text")
    try:
        encoded = value.encode("utf-8", "strict")
    except UnicodeError as exc:
        raise ConfigError(f"private WLAN {name} is not valid UTF-8 text") from exc
    if (not minimum <= len(encoded) <= maximum
            or any(byte < 0x20 or byte == 0x7f for byte in encoded)):
        raise ConfigError(f"private WLAN {name} has invalid length or control characters")
    return encoded


def _display_text(value: object, name: str, minimum: int, maximum: int) -> bytes:
    if not isinstance(value, str):
        raise ConfigError(f"private display {name} must be text")
    try:
        encoded = value.encode("utf-8", "strict")
    except UnicodeError as exc:
        raise ConfigError(f"private display {name} is not valid UTF-8 text") from exc
    if (not minimum <= len(encoded) <= maximum
            or any((byte < 0x20 and byte not in (0x0a, 0x0d)) or byte == 0x7f
                   for byte in encoded)):
        raise ConfigError(f"private display {name} has invalid length or control characters")
    return encoded


def _c_string(encoded: bytes) -> str:
    return '"' + "".join(f"\\{byte:03o}" for byte in encoded) + '"'


def _parse_profile(config_bytes: bytes) -> dict[str, object]:
    try:
        config = json.loads(config_bytes)
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ConfigError("external private profile is not valid JSON") from exc
    if not isinstance(config, dict) or set(config) != {
            "schema_version", "provenance", "wifi", "display",
            "legacy_personal_documentation_text"}:
        raise ConfigError("external private profile has an unexpected top-level schema")
    if config["schema_version"] != 1 or not isinstance(config["provenance"], str):
        raise ConfigError("external private profile metadata is invalid")
    wifi = config["wifi"]
    display = config["display"]
    if not isinstance(wifi, dict) or set(wifi) != {"ssid", "password"}:
        raise ConfigError("external private Wi-Fi profile schema is invalid")
    if not isinstance(display, dict) or set(display) != {
            "medication_alarm_text", "seltsam_morse_text"}:
        raise ConfigError("external private display profile schema is invalid")
    if not isinstance(config["legacy_personal_documentation_text"], str):
        raise ConfigError("external documentation profile field is invalid")
    return config


def _render_wifi(config_bytes: bytes | None) -> bytes:
    if config_bytes is None:
        ssid = password = b""
    else:
        config = _parse_profile(config_bytes)
        wifi = config["wifi"]
        assert isinstance(wifi, dict)
        ssid = _value(wifi["ssid"], "SSID", 1, 32)
        password = _value(wifi["password"], "password", 8, 63)
    text = (
        "#ifndef KATMED_PRIVATE_WIFI_DEFAULTS_H\n"
        "#define KATMED_PRIVATE_WIFI_DEFAULTS_H\n"
        f"#define KATMED_WIFI_DEFAULT_SSID {_c_string(ssid)}\n"
        f"#define KATMED_WIFI_DEFAULT_PASSWORD {_c_string(password)}\n"
        "#endif\n"
    )
    return text.encode("ascii")


def _render_ui_profile(config_bytes: bytes | None) -> bytes:
    lines = [
        "#ifndef KATMED_PRIVATE_UI_PROFILE_H",
        "#define KATMED_PRIVATE_UI_PROFILE_H",
    ]
    if config_bytes is not None:
        config = _parse_profile(config_bytes)
        display = config["display"]
        assert isinstance(display, dict)
        alarm = _display_text(display["medication_alarm_text"], "alarm text", 1, 200)
        seltsam = _display_text(display["seltsam_morse_text"], "Seltsam Morse text", 1, 4096)
        documentation = _value(config["legacy_personal_documentation_text"],
                                "documentation text", 1, 8192)
        del documentation  # Secured with the profile; never embedded in firmware.
        lines.extend((
            f"#define KATMED_DEFAULT_ALARM_TEXT {_c_string(alarm)}",
            f"#define KATMED_DEFAULT_SELTSAM_MORSE_TEXT {_c_string(seltsam)}",
        ))
    lines.append("#endif")
    return ("\n".join(lines) + "\n").encode("ascii")


def _write_private(output: pathlib.Path, content: bytes) -> None:
    directory = output.parent
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    for protected_directory in (directory, directory.parent):
        info = protected_directory.lstat()
        if (not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode)
                or info.st_uid != os.getuid()):
            raise ConfigError("generated WLAN header directories must be owned directories")
        os.chmod(protected_directory, 0o700)
    if output.exists() or output.is_symlink():
        existing = output.lstat()
        if not stat.S_ISREG(existing.st_mode) or existing.st_uid != os.getuid():
            raise ConfigError("generated WLAN header destination is not an owned regular file")
    descriptor, temporary_name = tempfile.mkstemp(prefix=".wifi-", dir=directory)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, output)
        os.chmod(output, 0o600)
        directory_fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        try:
            os.unlink(temporary_name)
        except OSError:
            pass
        raise


def generate(config_path: pathlib.Path | None, output_path: pathlib.Path,
             ui_profile_path: pathlib.Path | None = None) -> None:
    config_bytes = None if config_path is None else _private_regular_file(config_path)
    _write_private(output_path, _render_wifi(config_bytes))
    if ui_profile_path is not None:
        _write_private(ui_profile_path, _render_ui_profile(config_bytes))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=pathlib.Path,
                        help="explicit external private Wi-Fi profile; omit for neutral standard build")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--ui-profile-output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        generate(args.config, args.output, args.ui_profile_output)
    except (ConfigError, OSError) as exc:
        print(f"private WLAN defaults error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
