#!/usr/bin/env python3
"""Apply the bounded atomic-write bridge and numeric probe hooks to ACME 1.1.1.

The component manager owns the extracted source. This bounded, idempotent
step runs in the private build tree after resolution and refuses any source
that is not the verified 1.1.1 payload. It never changes the SDK or a
component outside the current firmware build tree.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys


EXPECTED = {
    "Acme.cpp": "acaef64533ab8c194f9bdf958ccd32df3fcc3de6c00e67ab12fa05e6c398c595",
    "Cache.cpp": "7d843c8297f16eb9f0ac2761330f3c49c4093fe2c9c9e6c5962830cd0fe31ceb",
    "Crypto.cpp": "a5e479237b4a5b1560068b7f85aa568b3aaafb3c4b0aa84c39630f5d3351b9f3",
}
MARKER = "KATMED_ACME_ATOMIC_PATCH_1"
PROBE_MARKER = "KATMED_ACME_RESOURCE_HOOK_1"
RESOURCE_FIX_MARKER = "KATMED_ACME_HTTP01_FILE_FIX_1"
DECLARATION = """
#include <stdio.h>
extern \"C\" FILE *katmed_acme_atomic_open(const char *target);
extern \"C\" int katmed_acme_atomic_close(FILE *stream);
""".strip()


class PatchError(ValueError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _patch_source(path: Path, writes: tuple[str, ...], closes: tuple[str, ...],
                  expected_counts: dict[str, int]) -> None:
    original = path.read_text(encoding="utf-8")
    if MARKER in original:
        updated = original
        if "#include <stdio.h>" not in updated:
            updated = updated.replace("/* " + MARKER + " */",
                                      "#include <stdio.h>\n\n/* " + MARKER + " */", 1)
        updated = updated.replace('katmed_acme_atomic_open(fn, "w")',
                                  "katmed_acme_atomic_open(fn)")
        updated = updated.replace('katmed_acme_atomic_open(localfn, "w")',
                                  "katmed_acme_atomic_open(localfn)")
        if updated != original:
            path.write_text(updated, encoding="utf-8")
        return
    if not original.startswith("/*") or "#include" not in original:
        raise PatchError(f"official ACME source shape unknown: {path.name}")
    changed = original
    for old in writes:
        count = changed.count(old)
        expected = expected_counts.get(old, 1)
        if count != expected:
            raise PatchError(f"expected one write site in {path.name}")
        replacement = old.replace("fopen", "katmed_acme_atomic_open").replace(', "w"', "")
        changed = changed.replace(old, replacement, expected)
    for old in closes:
        count = changed.count(old)
        expected = expected_counts.get(old, 1)
        if count != expected:
            raise PatchError(f"expected one close site in {path.name}")
        changed = changed.replace(old, old.replace("fclose", "katmed_acme_atomic_close"), expected)
    include = '#include "esp_log.h"\n'
    if include not in changed:
        include = '#include "Acme.h"\n'
    if include not in changed:
        raise PatchError(f"ACME include anchor missing in {path.name}")
    changed = changed.replace(include, include + "\n/* " + MARKER + " */\n" + DECLARATION + "\n", 1)
    path.write_text(changed, encoding="utf-8")


def _patch_probe(path: Path, sites: tuple[tuple[str, str], ...]) -> None:
    """Add numeric observation hooks to the resolved copy, never the archive."""
    source = path.read_text(encoding="utf-8")
    if PROBE_MARKER in source:
        return
    declaration = (
        f'/* {PROBE_MARKER} */\n'
        'extern "C" void katmed_acme_probe_mark(unsigned phase) __attribute__((weak));\n'
        '#define KATMED_ACME_PROBE(p) do { if (katmed_acme_probe_mark) '
        'katmed_acme_probe_mark(p); } while (0)\n'
    )
    anchor = f'/* {MARKER} */\n'
    if source.count(anchor) != 1:
        raise PatchError(f"atomic patch anchor missing in {path.name}")
    source = source.replace(anchor, declaration + anchor, 1)
    for before, after in sites:
        if source.count(before) != 1:
            raise PatchError(f"ACME probe site changed in {path.name}")
        source = source.replace(before, after, 1)
    path.write_text(source, encoding="utf-8")


def _patch_resource_fixes(path: Path) -> None:
    """Keep the resolved client's HTTP-01 and certificate success paths fail-closed."""
    source = path.read_text(encoding="utf-8")
    if RESOURCE_FIX_MARKER in source:
        anchor = f"/* {RESOURCE_FIX_MARKER} */\n"
        includes = "#include <stdio.h>\n#include <esp_http_server.h>\n"
        if includes + anchor not in source:
            if source.count(anchor) != 1:
                raise PatchError("ACME resource fix marker changed")
            path.write_text(source.replace(anchor, includes + anchor, 1), encoding="utf-8")
        return

    def replace_once(before: str, after: str) -> None:
        nonlocal source
        if source.count(before) != 1:
            raise PatchError("ACME resource fix anchor changed")
        source = source.replace(before, after, 1)

    replace_once(
        f"/* {MARKER} */\n",
        '#include <stdio.h>\n#include <esp_http_server.h>\n'
        f'/* {RESOURCE_FIX_MARKER} */\n'
        'extern "C" bool katmed_acme_http_host_allowed(httpd_req_t *req);\n'
        'extern "C" int katmed_acme_atomic_abort(FILE *stream);\n'
        f"/* {MARKER} */\n",
    )
    replace_once(
        '  if (strcmp(req->uri, _acme->ValidationFile) == 0) {',
        '  if (!katmed_acme_http_host_allowed(req)) {\n'
        '    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "host");\n'
        '    return ESP_OK;\n'
        '  }\n'
        '  if (strcmp(req->uri, _acme->ValidationFile) == 0) {',
    )
    replace_once(
        '    free(ovf);\n    ovf = NULL;\n  }\n\n  memset((void *)&wsconf, 0, sizeof(wsconf));',
        '    free(ovf);\n    ovf = NULL;\n    ws_registered = false;\n'
        '  }\n\n  memset((void *)&wsconf, 0, sizeof(wsconf));',
    )
    replace_once(
        '  wsconf.uri = strdup(ValidationFile);\n  wsconf.method = HTTP_GET;',
        '  wsconf.uri = strdup(ValidationFile);\n'
        '  if (wsconf.uri == NULL) {\n'
        '    ESP_LOGE(tag, "%s: challenge URI allocation failed", __FUNCTION__);\n'
        '    return;\n'
        '  }\n'
        '  wsconf.method = HTTP_GET;',
    )
    replace_once(
        '      __FUNCTION__, wsconf.uri, err, esp_err_to_name(err));\n  } else {',
        '      __FUNCTION__, wsconf.uri, err, esp_err_to_name(err));\n'
        '    free((void *)wsconf.uri);\n'
        '  } else {',
    )
    replace_once(
        '    EnableLocalWebServer();\n  } else {',
        '    EnableLocalWebServer();\n'
        '    if (!ws_registered) {\n'
        '      free(ValidationString);\n      free(ValidationFile);\n'
        '      ValidationString = ValidationFile = NULL;\n'
        '      return status_fail;\n'
        '    }\n'
        '  } else {',
    )
    replace_once(
        '    ValidationFile = (char *)malloc(strlen(well_known) + strlen(token) + 2);\n'
        '    sprintf(ValidationFile, "%s%s", well_known, token);',
        '    ValidationFile = (char *)malloc(strlen(well_known) + strlen(token) + 2);\n'
        '    if (ValidationString == NULL || ValidationFile == NULL) {\n'
        '      free(ValidationString);\n      free(ValidationFile);\n'
        '      ValidationString = ValidationFile = NULL;\n'
        '      return status_fail;\n'
        '    }\n'
        '    sprintf(ValidationFile, "%s%s", well_known, token);',
    )
    replace_once(
        '    katmed_acme_atomic_close(f);\n  } else {',
        '    int committed = ok ? katmed_acme_atomic_close(f) : katmed_acme_atomic_abort(f);\n'
        '    if (committed != 0) {\n'
        '      ESP_LOGE(tag, "Certificate file commit failed");\n'
        '      ok = false;\n'
        '    }\n'
        '  } else {',
    )
    replace_once(
        '  free(reply);\n\n  if (ok) ReadCertificate();',
        '  free(fn);\n  free(reply);\n\n  if (ok) ReadCertificate();',
    )
    path.write_text(source, encoding="utf-8")


EXPECTED_HEADER = '5fe34f81a3a7f03270b0b30ae5b2bba6293c2d0610a7ecdd5e6928930091e1ff'
LIFECYCLE_MARKER = "KATMED_ACME_LIFECYCLE_4"

def _patch_lifecycle(component: Path) -> None:
    path = component / "Acme.cpp"
    source = path.read_text()
    header = component / "include/Acme.h"
    h = header.read_text()
    original_header = h.replace("  public:\n    bool Cleanup();", "  public:", 1)
    if hashlib.sha256(original_header.encode()).hexdigest() != EXPECTED_HEADER:
        raise PatchError("official ACME header checksum mismatch")
    if LIFECYCLE_MARKER in source:
        if "bool Cleanup();" not in h:
            raise PatchError("ACME Cleanup declaration missing")
        return
    def replace(before, after):
        nonlocal source
        if source.count(before) != 1:
            raise PatchError("ACME lifecycle anchor changed")
        source = source.replace(before, after, 1)
    replace('Acme::~Acme() {', 'Acme::~Acme() {\n  Cleanup();\n  if (_acme == this) _acme = NULL;')
    replace('  if (!katmed_acme_http_host_allowed(req)) {',
            '  if (_acme == NULL || _acme->ValidationFile == NULL ||\n      _acme->ValidationString == NULL || !katmed_acme_http_host_allowed(req)) {')
    replace('  httpd_unregister_uri_handler(webserver, ovf, HTTP_GET);\n  free(ovf);',
            '  if (httpd_unregister_uri_handler(webserver, ovf, HTTP_GET) != ESP_OK) return;\n  free(ovf);')
    # Reuse Disable rather than losing the old registration on failure.
    start = source.index('  if (ws_registered && (ovf != NULL)) {')
    end = source.index('\n\n  memset((void *)&wsconf', start)
    source = source[:start] + '  if (ws_registered) {\n    DisableLocalWebServer();\n    if (ws_registered) return;\n  }' + source[end:]
    replace('    ValidationString = CreateValidationString(token);',
            '    if (!Cleanup()) return status_fail;\n    ValidationString = CreateValidationString(token);')
    replace("""    if (r != status_pending) {
      if (ValidationString) {
        free(ValidationString);
        ValidationString = 0;
      }
      if (ValidationFile) {
        free(ValidationFile);
        ValidationFile = 0;
      }

      if (ws_registered)
        DisableLocalWebServer();
    }""", '    if (r != status_pending && !Cleanup()) return status_fail;')
    replace('bool Acme::DownloadCertificate() {',
            'bool Acme::DownloadCertificate() {\n  KATMED_ACME_PROBE(8);\n'
            '  struct Window { ~Window() { KATMED_ACME_PROBE(10); } } window;')
    replace('  free(msg);\n  if (reply) {\n    ESP_LOGD(tag, "%s -> %s", __FUNCTION__, reply);',
            '  free(msg);\n  KATMED_ACME_PROBE(9);\n  if (reply) {\n    ESP_LOGD(tag, "%s -> %s", __FUNCTION__, reply);')
    replace('    int committed = ok ?', '    KATMED_ACME_PROBE(9);\n    int committed = ok ?')
    replace('  if (ok) ReadCertificate();', '  KATMED_ACME_PROBE(9);\n  if (ok) ReadCertificate();')
    # Serialize every deregistration, including normal validation completion,
    # with HTTP request execution. Never free URI/data while send uses them.
    start = source.index('void Acme::DisableLocalWebServer() {')
    end = source.index('\n}\n', start) + 3
    source = source[:start] + r'''
void Acme::DisableLocalWebServer() {
  if (!ws_registered) return;
  if (!webserver || !ovf) return;
  struct Work { Acme *self; SemaphoreHandle_t done; };
  StaticSemaphore_t storage;
  Work work = {this, xSemaphoreCreateBinaryStatic(&storage)};
  auto disable = [](void *arg) {
    Work *w = static_cast<Work *>(arg);
    Acme *a = w->self;
    if (httpd_unregister_uri_handler(a->webserver, a->ovf, HTTP_GET) == ESP_OK) {
      free(a->ovf); a->ovf = NULL;
      a->ws_registered = false;
    }
    xSemaphoreGive(w->done);
  };
  if (httpd_queue_work(webserver, disable, &work) != ESP_OK) return;
  xSemaphoreTake(work.done, portMAX_DELAY);
}
''' + source[end:]
    source += r'''
/* KATMED_ACME_LIFECYCLE_4
 * Disable waits for the server's serialized work queue, after active requests.
 * Callers retain the object and writer lease when deregistration fails.
 */
bool Acme::Cleanup() {
  DisableLocalWebServer();
  if (ws_registered) return false;
  free(ovf); ovf = NULL;
  free(ValidationFile); ValidationFile = NULL;
  free(ValidationString); ValidationString = NULL;
  return true;
}
'''
    source = '#include "freertos/FreeRTOS.h"\n#include "freertos/semphr.h"\n' + source
    if h.count('  public:') != 1:
        raise PatchError("ACME public anchor changed")
    header.write_text(h.replace('  public:', '  public:\n    bool Cleanup();', 1))
    path.write_text(source)


def apply(component: Path) -> bool:
    component = component.resolve()
    manifest = component / "idf_component.yml"
    if not component.is_dir() or not manifest.is_file():
        raise PatchError("resolved dannybackx/acmeclient component is missing")
    manifest_text = manifest.read_text(encoding="utf-8")
    if "version: 1.1.1" not in manifest_text:
        raise PatchError("resolved ACME component is not version 1.1.1")
    paths = {name: component / name for name in EXPECTED}
    for name, path in paths.items():
        if not path.is_file():
            raise PatchError(f"official ACME source is missing: {name}")
        if MARKER not in path.read_text(encoding="utf-8") and _sha256(path) != EXPECTED[name]:
            raise PatchError(f"official ACME source checksum mismatch: {name}")
    changed = False
    paths["include/Acme.h"] = component / "include/Acme.h"
    before = {name: path.read_bytes() for name, path in paths.items()}
    try:
        _patch_source(paths["Cache.cpp"],
                      ('FILE *f = fopen(fn, "w");',),
                      ("fclose(f);",),
                      {'FILE *f = fopen(fn, "w");': 4, "fclose(f);": 4})
        _patch_source(paths["Crypto.cpp"],
                      ('FILE *f = fopen(fn, "w");',),
                      ("fclose(f);", "fclose(f) != 0"), {})
        _patch_source(paths["Acme.cpp"],
                      ('FILE *f = fopen(fn, "w");', 'FILE *tf = fopen(localfn, "w");'),
                      ("fclose(f);", "fclose(tf);"), {})
        _patch_probe(paths["Crypto.cpp"], (
            ('void Acme::GenerateAccountKey() {',
             'void Acme::GenerateAccountKey() {\n  KATMED_ACME_PROBE(1);'),
            ('void Acme::GenerateCertificateKey() {',
             'void Acme::GenerateCertificateKey() {\n  KATMED_ACME_PROBE(1);'),
        ))
        _patch_probe(paths["Acme.cpp"], (
            ('bool Acme::RequestNewNonce() {',
             'bool Acme::RequestNewNonce() {\n  KATMED_ACME_PROBE(2);'),
            ('char *Acme::PerformWebQuery(const char *query, const char *topost, const char *apptype, const char *accept_message) {',
             'char *Acme::PerformWebQuery(const char *query, const char *topost, const char *apptype, const char *accept_message) {\n  KATMED_ACME_PROBE(2);'),
            ('Acme::return_status Acme::ValidateOrder() {',
             'Acme::return_status Acme::ValidateOrder() {\n  KATMED_ACME_PROBE(3);'),
            ('void Acme::FinalizeOrder() {',
             'void Acme::FinalizeOrder() {\n  KATMED_ACME_PROBE(4);'),
            ('void Acme::RenewCertificate() {',
             'void Acme::RenewCertificate() {\n  KATMED_ACME_PROBE(5);'),
            ('    if (ok)\n      return true;\n  }\n\n  if (strcmp(order->status, acme_status_invalid)',
             '    if (ok) {\n      KATMED_ACME_PROBE(6);\n      return true;\n    }\n  }\n\n  if (strcmp(order->status, acme_status_invalid)'),
        ))
        _patch_resource_fixes(paths["Acme.cpp"])
        _patch_lifecycle(component)
        changed = any(path.read_bytes() != before[name] for name, path in paths.items())
    except Exception:
        for name, path in paths.items():
            path.write_bytes(before[name])
        raise
    return changed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--component", required=True, type=Path)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    try:
        changed = apply(args.component)
    except (OSError, PatchError) as exc:
        print(f"ACME-Atomic-Patch abgebrochen: {exc}", file=sys.stderr)
        return 1
    if not args.quiet:
        print("ACME-Atomic-Patch: " + ("angewendet" if changed else "bereits aktiv"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
