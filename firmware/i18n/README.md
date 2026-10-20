# Product copy and display glyphs

`de.json` lists the authored UI keys. Each other JSON file maps every key in
`display`, `web_static`, and `web_dynamic` to one translation. Technical
identifiers, user alarm text, device names, SSIDs, and addresses are data and
must stay outside this catalog. The Store holds only a one byte language ID.

The catalog currently contains German, English, French, Dutch, Ukrainian,
Arabic, simplified Chinese, Spanish, Japanese, and Esperanto. Ostfränkisch
(`bar`) and Latin Klingon (`tlh-Latn`) are deliberately unavailable until
genuine complete translations exist. The full gate
`python tests/host_i18n/check_catalog.py` fails while either is absent.

After editing copy, run `python tests/host_i18n/check_catalog.py --available`
and regenerate the C and browser catalogs with `tools/build_i18n_assets.py`.
`tools/build_i18n_font.py` emits the 12 and 14 pixel LVGL fallback fonts for
the static display catalog. It uses the pinned Noto Sans CJK SC Regular 2.004
source and locally installed Noto Sans Arabic; the OFL licenses are included
beside this file. The generated font bitmaps are uncompressed because the
firmware does not enable LVGL's compressed font decoder. The subsets cover
product copy; they do not promise arbitrary user supplied Unicode glyphs.

The locale dictionaries began as machine translation drafts. Their keys,
placeholders, and formats are checked automatically, while linguistic review
remains necessary, especially for reset and safety text.
