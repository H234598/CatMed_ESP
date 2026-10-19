#!/usr/bin/env python3
"""Silent privacy gate for neutral and explicitly profiled firmware artifacts."""

import argparse
import json
import pathlib
import sys


def read_profile(path: pathlib.Path) -> dict[str, bytes]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return {
        "wifi_ssid": data["wifi"]["ssid"].encode("utf-8"),
        "wifi_password": data["wifi"]["password"].encode("utf-8"),
        "alarm_text": data["display"]["medication_alarm_text"].encode("utf-8"),
        "seltsam_text": data["display"]["seltsam_morse_text"].encode("utf-8"),
        "legacy_documentation": data["legacy_personal_documentation_text"].encode("utf-8"),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", required=True, type=pathlib.Path)
    parser.add_argument("--neutral", required=True, nargs="+", type=pathlib.Path)
    parser.add_argument("--private", required=True, nargs="+", type=pathlib.Path)
    args = parser.parse_args()
    fields = read_profile(args.profile)
    try:
        neutral = [(path, path.read_bytes()) for path in args.neutral]
        private = [(path, path.read_bytes()) for path in args.private]
    except OSError:
        print("privacy_fields=5 neutral_absent=0 private_consumed=0 artifacts=0 status=fail")
        return 1

    neutral_checks = 0
    private_checks = 0
    passed = True
    # Every private field must be absent from both neutral board artifacts.
    for _, artifact in neutral:
        for value in fields.values():
            neutral_checks += 1
            passed &= value not in artifact
    # Selected private builds consume WLAN/display values; documentation stays
    # outside firmware even for a private profile build.
    for _, artifact in private:
        for key in ("wifi_ssid", "wifi_password", "alarm_text", "seltsam_text"):
            private_checks += 1
            passed &= fields[key] in artifact
        private_checks += 1
        passed &= fields["legacy_documentation"] not in artifact

    status = "pass" if passed else "fail"
    print(f"privacy_fields={len(fields)} neutral_absent={neutral_checks} "
          f"private_consumed={private_checks} artifacts={len(neutral)+len(private)} "
          f"status={status}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
