#!/usr/bin/env python3
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
source = (root / "firmware/components/katmed_ui/ui.c").read_text()
settings_start = source.index("static void build_settings(void)\n{")
advanced_fn = source.index("static void advanced_button(lv_event_t *event)\n{")
assert 'label(ui.settings, "Advanced", row_w);' in source
assert 'button(ui.settings, "Advanced", row_w, 34, advanced_button, NULL);' in source
advanced = source[source.index('if (ui.advanced_open) {', settings_start):advanced_fn]
for field in ("Medikamente: Stunden", "Bilder: Stunden", "Nächste Fälligkeit HH:MM"):
    assert f'button(ui.settings, "{field}"' in advanced
assert 'restore_scroll(ui.settings, 9);' in advanced
assert 'restore_scroll(ui.settings, 5);' in source[settings_start:advanced_fn]
advanced_action = source[advanced_fn:source.index('static void ', advanced_fn + 20)]
assert 'remember_scroll(ui.settings, ui.advanced_open ? 9u : 5u);' in advanced_action
assert 'ui.advanced_open = !ui.advanced_open;' in advanced_action
assert 'build_settings();' in advanced_action
touch = source[source.index('uint64_t duration = monotonic_ms - ui.down_ms;'):source.index('if (!ui.touch_armed)', source.index('uint64_t duration = monotonic_ms - ui.down_ms;'))]
assert 'ui.view == SETTINGS && ui.advanced_open && dx >= 40' in touch
assert 'remember_scroll(ui.settings, 9);' in touch
assert 'ui.advanced_open = false;' in touch
assert 'build_settings();' in touch
settings = source[settings_start:advanced_fn]
assert not re.search(r'button\(ui\.settings,\s*"(?:Status|Zurück|Zuruck)', settings)
status = source[source.index('static void status_button(lv_event_t *event)\n{'):]
status = status[:status.index('/* Stable status rows')]
assert not re.search(r'button\(ui\.status,\s*"(?:Status|Zurück|Zuruck)', status)
print("native Advanced heading/button, subpage, swipe, scroll retention and removed settings/status controls passed")
