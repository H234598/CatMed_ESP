#!/usr/bin/env python3
"""Emit checked-in LVGL and browser catalogs from language JSON.

Use --check to verify generated artifacts. Otherwise stdout is the requested
artifact, so source generation never changes the repository implicitly.
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAT = ROOT / "firmware/i18n"
ORDER = ("en", "de", "fr", "nl", "uk", "ar", "zh-Hans", "es", "ja", "bar", "eo", "tlh-Latn")
SOURCE = json.loads((CAT / "de.json").read_text())
LOCALES = {code: json.loads((CAT / f"{code}.json").read_text())
           for code in ORDER if (CAT / f"{code}.json").exists() and code != "de"}


def cstr(text):
    return json.dumps(text, ensure_ascii=False)


def display():
    rows = []
    for key in SOURCE["display"]:
        values = [cstr(key)]
        for code in ORDER:
            if code == "de":
                values.append(cstr(key))
            elif code in LOCALES:
                values.append(cstr(LOCALES[code]["display"][key]))
            else:
                values.append("NULL")
        rows.append("    {" + values[0] + ", {" + ", ".join(values[1:]) + "}},")
    return '''/* Generated from firmware/i18n JSON by tools/build_i18n_assets.py. */
#include "katmed_i18n.h"
#include <stdatomic.h>
#include <string.h>

static atomic_uchar selected_language = KATMED_LANG_EN;
typedef struct { const char *source; const char *value[KATMED_LANGUAGE_COUNT]; } Entry;
static const Entry entries[] = {
''' + "\n".join(rows) + '''
};

bool katmed_i18n_available(uint8_t language) {
    return language < KATMED_LANGUAGE_COUNT && entries[0].value[language] != NULL;
}
void katmed_i18n_select(uint8_t language) {
    if (katmed_i18n_available(language))
        atomic_store(&selected_language, language);
}
uint8_t katmed_i18n_selected(void) { return atomic_load(&selected_language); }
const char *katmed_i18n_text(const char *source) {
    if (!source) return "";
    const uint8_t language = katmed_i18n_selected();
    if (language == KATMED_LANG_DE) return source;
    for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]); ++i)
        if (strcmp(entries[i].source, source) == 0)
            return entries[i].value[language] ? entries[i].value[language] : source;
    return source;
}
'''


def browser():
    keys = list(SOURCE["web_static"]) + list(SOURCE["web_dynamic"])
    data = {"keys": keys, "values": {}}
    for code in ORDER:
        if code == "de":
            data["values"][code] = None  # Preserve locale order, alias source keys below.
            continue
        elif code in LOCALES:
            data["values"][code] = [LOCALES[code]["web_static"].get(key,
                                     LOCALES[code]["web_dynamic"].get(key)) for key in keys]
    return "/* Generated from firmware/i18n JSON. */\nwindow.KATMED_I18N = " + \
        json.dumps(data, ensure_ascii=False, separators=(",", ":")) + \
        ";\nwindow.KATMED_I18N.values.de = window.KATMED_I18N.keys;\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("target", choices=("display", "browser"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    result = display() if args.target == "display" else browser()
    output = ROOT / ("firmware/components/katmed_ui/i18n_catalog.c" if args.target == "display"
                     else "firmware/components/katmed_web/ui/i18n.js")
    if args.check:
        if not output.exists() or output.read_text() != result:
            raise SystemExit(f"stale generated catalog: {output}")
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
