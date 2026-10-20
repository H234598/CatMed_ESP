#!/usr/bin/env python3
"""Extract product-authored UI copy for catalog maintenance.

The output is a review input, not an automatic translation or a runtime parser.
Protocol fields and user data are deliberately outside this inventory.
"""
import json
import re
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "firmware/components/katmed_ui"
WEB = ROOT / "firmware/components/katmed_web/ui"
STRINGS = re.compile(r'"((?:[^"\\]|\\.)*)"')


def c_strings(path):
    values = []
    for line in path.read_text().splitlines():
        if line.lstrip().startswith("#"):
            continue
        for raw in STRINGS.findall(line):
            try:
                value = json.loads('"' + raw + '"')
            except json.JSONDecodeError:
                continue
            if re.search(r"[A-Za-zÄÖÜäöüß]", value):
                values.append(value)
    return values


class HtmlCopy(HTMLParser):
    def __init__(self):
        super().__init__()
        self.values = []

    def handle_data(self, data):
        if data.strip():
            self.values.append(data.strip())

    def handle_starttag(self, _tag, attrs):
        self.values.extend(value for name, value in attrs
                           if name in ("placeholder", "title", "aria-label") and value)


def unique(values):
    return list(dict.fromkeys(values))


def collect():
    ui = unique(c_strings(UI / "ui.c") + c_strings(UI / "clock_view.c"))
    preview_fragments = ["%02u:%02u heute nicht vorhanden\n",
                         "Nächster %s\n",
                         "Alarm %s\n%s%s"]
    ui = ["".join(preview_fragments) if s == preview_fragments[0] else s
          for s in ui if s not in preview_fragments[1:]]
    ui = [s for s in ui if s not in {
        "CEST", "CET", "KatmedESP", "HH:MM", "0°", "90°", "180°", "270°",
        "%s", "%lu", "OK", "Telacore", "Alarm", "IP: %s", "AP-IP: %s",
        "Gateway: %s", "DNS-IP: %s", "%c%c:%s",
        "English", "Deutsch", "Français", "Nederlands", "Español", "Esperanto",
        "Українська", "العربية", "简体中文", "日本語",
    } and not re.fullmatch(r"[%0-9.:usdlu\s-]+", s)]
    html = HtmlCopy()
    html.feed((WEB / "index.html").read_text())
    static = unique(html.values)
    static = [s for s in static if s not in {
        "KatMed", "0°", "90°", "180°", "270°", "SSID", "HH:MM",
        "English", "Deutsch", "Français", "Nederlands", "Español", "Esperanto",
        "Українська", "العربية", "简体中文", "日本語",
    }]
    js = unique(c_strings(WEB / "app.js"))
    js = [s for s in js if (" " in s or re.search(r"[ÄÖÜäöüß…]", s) or
                            s in {"Bearbeiten", "Entfernen", "Freigegeben."})
          and s not in {"use strict", "HTTP request failed", "PRIVATE KEY",
                        "-----BEGIN CERTIFICATE-----", "same-origin", "no-store"}
          and not s.startswith("Invalid ")]
    js = [s for s in js if s != "Readback differs from committed configuration"]
    templates = [
        "Gerät: {0}",
        "Boot-Alarm aktiv; gespeicherter Termin {0} pausiert.",
        "Aktuell: {0}",
        "Freigegeben: {0}",
        "{0} von {1} WLAN-Profilen belegt. Priorität 1 ist die höchste.",
        "Priorität {0} · {1} · {2}",
        "Termin: {0}. Wirksam: {1}.",
    ]
    return {"display": ui, "web_static": static, "web_dynamic": js + templates}


if __name__ == "__main__":
    print(json.dumps(collect(), ensure_ascii=False, indent=2))
