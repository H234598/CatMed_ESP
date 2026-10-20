#!/usr/bin/env python3
"""Check product-copy coverage and interpolation safety across UI and Web."""
import argparse
import importlib.util
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CAT = ROOT / 'firmware/i18n'
ORDER = ('en', 'de', 'fr', 'nl', 'uk', 'ar', 'zh-Hans', 'es', 'ja', 'bar', 'eo', 'tlh-Latn')
PRINTF = re.compile(r'%(?:[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|t|j)?[diouxXfFeEgGaAcspn%])')
WEB_FORMAT = re.compile(r'\{\d+\}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--available', action='store_true', help='verify currently installed packages while listing missing ones')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('sources', ROOT / 'tools/i18n_sources.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    source = json.loads((CAT / 'de.json').read_text())
    extracted = module.collect()
    assert {part: set(keys) for part, keys in source.items()} == {
        part: set(keys) for part, keys in extracted.items()}, 'catalog/source drift'
    missing = [code for code in ORDER if not (CAT / f'{code}.json').exists()]
    for code in ORDER:
        path = CAT / f'{code}.json'
        if not path.exists():
            continue
        if code == 'de':
            continue
        values = json.loads(path.read_text())
        assert values.keys() == source.keys(), (code, 'sections')
        for section, keys in source.items():
            assert set(values[section]) == set(keys), (code, section, 'keys')
            for key, value in values[section].items():
                assert isinstance(value, str) and value.strip(), (code, section, key, 'empty')
                assert value[:1].isspace() == key[:1].isspace(), (code, section, key, 'leading space')
                assert value[-1:].isspace() == key[-1:].isspace(), (code, section, key, 'trailing space')
                assert PRINTF.findall(value) == PRINTF.findall(key), (code, section, key, 'printf')
                assert sorted(WEB_FORMAT.findall(value)) == sorted(WEB_FORMAT.findall(key)), (code, section, key, 'web format')
    print('verified locales', len(ORDER) - len(missing), 'missing', ','.join(missing) or 'none')
    if missing and not args.available:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
