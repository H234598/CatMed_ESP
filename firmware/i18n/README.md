# Product copy and display glyphs

`de.json` lists the authored UI keys. Each other JSON file maps every key in
`display`, `web_static`, and `web_dynamic` to one translation. Technical
identifiers, user alarm text, device names, SSIDs, and addresses are data and
must stay outside this catalog. The Store holds only a one byte language ID.

The catalog contains German, English, French, Dutch, Ukrainian, Arabic,
simplified Chinese, Spanish, Japanese, Ostfränkisch (`bar`), Esperanto, and
Latin Klingon (`tlh-Latn`). The full gate
`python tests/host_i18n/check_catalog.py` requires all twelve packages.

After editing copy, run `python tests/host_i18n/check_catalog.py`
and regenerate the C and browser catalogs with `tools/build_i18n_assets.py`.
`tools/build_i18n_font.py` emits the 12 and 14 pixel LVGL fallback fonts for
the static display catalog. It uses the pinned Noto Sans CJK SC Regular 2.004
source, locally installed Noto Sans Arabic, and a hash-pinned local Noto Sans
Regular for Latin/Cyrillic glyphs absent from the CJK font. Noto Sans Regular
is Copyright 2022 The Noto Project Authors and uses the included SIL OFL 1.1;
the Arabic font uses the included Apache 2.0 license. The generated font
bitmaps are uncompressed because the
firmware does not enable LVGL's compressed font decoder. The subsets cover
product copy; they do not promise arbitrary user supplied Unicode glyphs.

The locale dictionaries began as machine translation drafts. Their keys,
placeholders, and formats are checked automatically, while linguistic review
remains necessary, especially for reset and safety text.
