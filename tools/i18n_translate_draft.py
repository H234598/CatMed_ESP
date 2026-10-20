#!/usr/bin/env python3
"""Print machine translation drafts for human review, without editing files.

This development helper is deliberately outside the firmware runtime. Catalogs
checked into the repository are the only strings shipped on a device.
"""
import concurrent.futures
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = json.loads((ROOT / "firmware/i18n/de.json").read_text())
LANGUAGE = sys.argv[1]
LANGS = {"en": "en", "fr": "fr", "nl": "nl", "uk": "uk", "ar": "ar",
         "zh-Hans": "zh-CN", "es": "es", "ja": "ja", "eo": "eo"}
if LANGUAGE not in LANGS:
    raise SystemExit("unsupported draft language")

VALUES = list(dict.fromkeys(value for section in SOURCE.values() for value in section))
PLACEHOLDERS = re.compile(r"%(?:[-+ #0]*\d*(?:\.\d+)?)?(?:hh|ll|l|z)?[diuoxXfFeEgGaAcspn%]|\{\d+\}")
SEPARATOR = "\n<<<00987654321>>>\n"
BREAK = "<<<00987654322>>>"


def translate(batch):
    text = SEPARATOR.join(item.replace("\n", BREAK) for item in batch)
    params = urllib.parse.urlencode({"client": "gtx", "sl": "de", "tl": LANGS[LANGUAGE],
                                     "dt": "t", "q": text})
    url = "https://translate.googleapis.com/translate_a/single?" + params
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=15) as response:
                data = json.load(response)
            result = "".join(fragment[0] for fragment in data[0])
            parts = re.split(r"\s*<<<\s*00987654321\s*>>>\s*", result)
            if len(parts) != len(batch):
                raise ValueError(f"translated {len(parts)} of {len(batch)} entries: {result[:100]!r}")
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


parts = list(batches(VALUES))
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    translated = [value for group in pool.map(translate, parts) for value in group]
lookup = dict(zip(VALUES, translated))
result = {section: {source: lookup[source] for source in values}
          for section, values in SOURCE.items()}
print(json.dumps(result, ensure_ascii=False, indent=2))
