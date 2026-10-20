#!/usr/bin/env python3
"""Emit an LVGL bitmap fallback for translated display copy.

Build input: locally installed OFL Noto Sans CJK SC and Noto Sans Arabic.
The checked-in result is generated code; no runtime font loader is needed.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import unicodedata
from pathlib import Path
from urllib.request import urlopen

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
CJK = Path.home() / '.cache/katmed-i18n-fonts/NotoSansCJKsc-Regular.otf'
CJK_URL = ('https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/'
           'Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf')
CJK_SHA256 = '2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b'
ARABIC = Path('/usr/share/fonts/google-noto-vf/NotoSansArabic[wght].ttf')


def converter_path():
    override = os.environ.get('KATMED_LV_FONT_CONV')
    if override:
        return Path(override)
    cached = sorted((Path.home() / '.npm/_npx').glob('*/node_modules/lv_font_conv/lv_font_conv.js'))
    if not cached:
        raise RuntimeError('lv_font_conv is not cached; set KATMED_LV_FONT_CONV')
    return cached[-1]


def render(size):
    if not CJK.exists():
        CJK.parent.mkdir(parents=True, exist_ok=True)
        with urlopen(CJK_URL, timeout=90) as response, CJK.open('wb') as output:
            while data := response.read(1024 * 1024):
                output.write(data)
    if CJK.stat().st_size != 16437364 or hashlib.sha256(CJK.read_bytes()).hexdigest() != CJK_SHA256:
        raise RuntimeError('unexpected Noto Sans CJK SC font version')
    strings = []
    for path in sorted((ROOT / 'firmware/i18n').glob('*.json')):
        document = json.loads(path.read_text())
        if path.stem == 'de':
            strings.extend(document['display'])
        else:
            strings.extend(document['display'].values())
    font = TTFont(CJK)  # SC includes the Japanese and Cyrillic code points in this catalog.
    cmap = font.getBestCmap()
    chars = ''.join(sorted({c for text in strings for c in text
                            if ord(c) > 127 and not 0x0600 <= ord(c) <= 0x06ff
                            and ord(c) in cmap}))
    arabic_base = {c for text in strings for c in text if 0x0600 <= ord(c) <= 0x06ff}
    arabic_forms = {chr(point) for point in list(range(0xfb50, 0xfe00)) + list(range(0xfe70, 0xff00))
                    if (normalized := unicodedata.normalize('NFKC', chr(point))) != chr(point)
                    and normalized and all(c in arabic_base for c in normalized)}
    arabic_chars = ''.join(sorted(arabic_base | arabic_forms))
    if not chars:
        raise RuntimeError('empty CJK/Unicode glyph subset')
    result = subprocess.run([
            'node', str(converter_path()), '--size', str(size), '--bpp', '2',
            '--format', 'lvgl', '--no-compress', '--no-kerning', '--font', str(CJK),
            '--symbols', chars, '--font', str(ARABIC),
            '--symbols', arabic_chars,
            '--lv-font-name', f'katmed_font_i18n_{size}', '-o', '/dev/stdout',
        ], check=False, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace')[-600:])
    return result.stdout.decode().replace('};\n\n\n\n#endif', '};\n\n#endif').rstrip() + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('size', type=int, choices=(12, 14))
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    result = render(args.size)
    output = ROOT / f'firmware/components/katmed_ui/katmed_font_i18n_{args.size}.c'
    if args.check:
        if not output.exists() or output.read_text() != result:
            raise SystemExit(f'stale generated font: {output}')
    else:
        sys.stdout.write(result)


if __name__ == '__main__':
    main()
