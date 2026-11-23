#!/usr/bin/env python3
"""Print machine translation drafts for human review, without editing files.

This development helper is deliberately outside the firmware runtime. Catalogs
checked into the repository are the only strings shipped on a device.
"""
import concurrent.futures
import http.client
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LANGS = {"en": "en", "fr": "fr", "nl": "nl", "uk": "uk", "ar": "ar",
         "zh-Hans": "zh-CN", "es": "es", "ja": "ja", "eo": "eo"}
MAX_RESPONSE_BYTES = 1024 * 1024
PLACEHOLDERS = re.compile(r"%(?:[-+ #0]*\d*(?:\.\d+)?)?(?:hh|ll|l|z)?[diuoxXfFeEgGaAcspn%]|\{\d+\}")
SEPARATOR = "\n<<<00987654321>>>\n"
BREAK = "<<<00987654322>>>"


def translate(batch, language):
    text = SEPARATOR.join(item.replace("\n", BREAK) for item in batch)
    params = urllib.parse.urlencode({"client": "gtx", "sl": "de", "tl": LANGS[language],
                                     "dt": "t", "q": text})
    url = "https://translate.googleapis.com/translate_a/single?" + params
    for attempt in range(4):
        try:
            try:
                # Fixed HTTPS endpoint; only query values vary.
                with urllib.request.urlopen(url, timeout=15) as response:  # nosec B310  # Fixed HTTPS endpoint; encoded query.
                    payload = response.read(MAX_RESPONSE_BYTES + 1)
            except http.client.IncompleteRead:
                raise ValueError("incomplete translation response") from None
            if len(payload) > MAX_RESPONSE_BYTES:
                raise ValueError("translation response too large")
            try:
                data = json.loads(payload)
            except ValueError:
                raise ValueError("invalid translation response JSON") from None
            if (not isinstance(data, list) or not data or not isinstance(data[0], list) or not data[0]
                    or not all(isinstance(fragment, list) and fragment
                               and isinstance(fragment[0], str) for fragment in data[0])):
                raise ValueError("invalid translation response structure")
            result = "".join(fragment[0] for fragment in data[0])
            parts = re.split(r"\s*<<<\s*00987654321\s*>>>\s*", result)
            if len(parts) != len(batch):
                raise ValueError(f"translated {len(parts)} of {len(batch)} entries")
            parts = [re.sub(r"<<<\s*00987654322\s*>>>", "\n", part)
                     for part in parts]
            for source, target in zip(batch, parts):
                if sorted(PLACEHOLDERS.findall(source)) != sorted(PLACEHOLDERS.findall(target)):
                    raise ValueError("format placeholder changed")
            return parts
        except (OSError, ValueError, IndexError):
            if attempt == 3:
                raise
            time.sleep(1 + attempt)


def batches(values):
    batch, size = [], 0
    for value in values:
        if batch and size + len(value) > 2400:
            yield batch
            batch, size = [], 0
        batch.append(value)
        size += len(value) + len(SEPARATOR)
    if batch:
        yield batch


def main():
    language = sys.argv[1]
    if language not in LANGS:
        raise SystemExit("unsupported draft language")
    source = json.loads((ROOT / "firmware/i18n/de.json").read_text())
    values = list(dict.fromkeys(value for section in source.values() for value in section))
    parts = list(batches(values))
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        translated = [value for group in pool.map(lambda batch: translate(batch, language), parts)
                      for value in group]
    lookup = dict(zip(values, translated))
    result = {section: {item: lookup[item] for item in items}
              for section, items in source.items()}
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
