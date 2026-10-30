"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const ui = path.resolve(__dirname, "../../firmware/components/katmed_web/ui");
const html = fs.readFileSync(path.join(ui, "index.html"), "utf8");
assert.match(html, /<h2 id="advanced-category-title">Erweitert<\/h2>/,
  "German advanced category heading must be localized");
assert.match(html, /<button id="open-advanced" type="button">Erweitert<\/button>/,
  "German advanced navigation control must be localized");
assert.match(html, /<h2 id="time-title">Erweitert<\/h2>/,
  "German advanced page heading must be localized");
for (const code of ["bar", "tlh-Latn"])
  assert.match(html, new RegExp(`<option value="${code}">`));
const nodes = [{ nodeValue: " Einstellungen " }, { nodeValue: "My own alarm text" }];
let previewNodeAttached = true;
let previewNodeValue = "Vor dem Speichern Vorschau prüfen.";
const previewNode = {
  get nodeValue() { return previewNodeValue; },
  set nodeValue(value) {
    previewNodeValue = value;
    if (previewNodeAttached) element("due-preview")._textContent = value;
  },
};
const fields = [{ attrs: { placeholder: "Passwort eingeben" },
  getAttribute(name) { return this.attrs[name] || null; },
  setAttribute(name, value) { this.attrs[name] = value; } }];
const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    id, value: "", dataset: {}, style: {}, classList: { toggle() {}, remove() {} },
    addEventListener(type, callback) {
      this.listeners ||= {};
      this.listeners[type] = callback;
    }, querySelector() { return null; }, querySelectorAll() { return []; },
    append() {}, replaceChildren() {}, reset() {}, close() {}, showModal() {},
  });
  return elements.get(id);
}
Object.defineProperty(element("due-preview"), "textContent", {
  get() { return this._textContent; },
  set(value) {
    this._textContent = value;
    previewNodeAttached = false;
  },
});
const document = {
  title: "KatMed · Einstellungen", documentElement: { lang: "de", dir: "ltr" },
  body: { classList: { remove() {} } },
  getElementById: element,
  createElement: () => element(`new-${elements.size}`),
  querySelectorAll: (selector) => selector === "[placeholder],[title],[aria-label]" ? fields : [],
  createTreeWalker() {
    const textNodes = [...nodes, previewNode];
    let index = 0;
    return { nextNode: () => textNodes[index++] || null };
  },
};
const setupReplies = [];
let languageReply = null;
let languagePutStatus = 200;
function jsonResponse(status, payload) {
  return {ok: status >= 200 && status < 300, status,
    headers: {get: () => "application/json"}, json: async () => payload};
}
const window = { addEventListener() {}, scrollY: 0, scrollTo() {} };
const context = { window, document, NodeFilter: { SHOW_TEXT: 4 },
  history: { pushState() {}, replaceState() {} },
  location: { hostname: "localhost", protocol: "https:", hash: "", pathname: "/", search: "" },
  setTimeout() {}, fetch: async (url, options = {}) => {
    if (url === "/api/config" && options.method !== "PUT")
      return jsonResponse(503, {error: "unavailable"});
    if (url === "/api/runtime-setup") {
      const reply = setupReplies.shift();
      assert.ok(reply, "unexpected runtime-setup request");
      return jsonResponse(200, reply);
    }
    if (url === "/api/language" && options.method === "PUT")
      return jsonResponse(languagePutStatus,
        languagePutStatus === 200 ? {language: languageReply} : {error: "commit_unknown"});
    if (url === "/api/wifi")
      return jsonResponse(503, {error: "wifi_unavailable"});
    if (url === "/api/acme-identifier" || url === "/api/client-cert" ||
        url.startsWith("/api/images") || url.startsWith("/api/catalog/images"))
      return jsonResponse(404, {error: "not_found"});
    throw new Error(`unexpected request path: ${url}`);
  } };
vm.runInNewContext(fs.readFileSync(path.join(ui, "i18n.js"), "utf8"), context);
const app = fs.readFileSync(path.join(ui, "app.js"), "utf8")
  .replace("function applyLocale(language) {", "window.__applyLocale = applyLocale; function applyLocale(language) {")
  .replace("function resetPreview() {", "window.__resetPreview = resetPreview; function resetPreview() {")
  .replace("async function applyBootstrap(bootstrap) {", "window.__applyBootstrap = applyBootstrap; async function applyBootstrap(bootstrap) {")
  .replace("function renderConfig(config) {", "window.__renderConfig = renderConfig; function renderConfig(config) {")
  .replace("const catalog = window.KATMED_I18N", "window.__state = state; const catalog = window.KATMED_I18N")
  .replace("async function loadRuntimeEinrichtung() {", "window.__loadRuntimeSetup = loadRuntimeEinrichtung; async function loadRuntimeEinrichtung() {")
  .replace("function trf(source, ...values) {", "window.__trf = trf; function trf(source, ...values) {")
  .replace("function formatPreviewDate(value) {", "window.__formatPreviewDate = formatPreviewDate; function formatPreviewDate(value) {")
  .replace("void start();", "");
vm.runInNewContext(app, context);
assert.equal(document.documentElement.lang, "en");
assert.equal(nodes[0].nodeValue.trim(), "Settings");
assert.equal(nodes[1].nodeValue, "My own alarm text");
assert.equal(window.__formatPreviewDate("20.10.2026 08:30 CEST"), "2026-10-20 08:30 CEST");
window.__resetPreview();
assert.equal(element("due-preview").textContent,
  window.KATMED_I18N.values.en[window.KATMED_I18N.keys.indexOf("Vor dem Speichern Vorschau prüfen.")]);
window.__applyLocale("de");
window.__resetPreview();
assert.equal(element("due-preview").textContent,
  window.KATMED_I18N.values.de[window.KATMED_I18N.keys.indexOf("Vor dem Speichern Vorschau prüfen.")]);
assert.equal(nodes[1].nodeValue, "My own alarm text");
const supported = ["en", "de", "fr", "nl", "uk", "ar", "zh-Hans", "es", "ja", "bar", "eo", "tlh-Latn"];
assert.deepEqual(Object.keys(window.KATMED_I18N.values), supported);
assert.strictEqual(window.KATMED_I18N.values.de, window.KATMED_I18N.keys);
for (const code of supported) {
  window.__applyLocale(code);
  assert.equal(document.documentElement.lang, code);
  assert.equal(document.documentElement.dir, code === "ar" ? "rtl" : "ltr");
  assert.equal(nodes[0].nodeValue.trim(), window.KATMED_I18N.values[code][
    window.KATMED_I18N.keys.indexOf("Einstellungen")]);
  assert.equal(nodes[1].nodeValue, "My own alarm text");
  assert.notEqual(nodes[0].nodeValue.trim(), "", code);
  const dynamicKey = "Gerät: {0}";
  const translatedDynamic = window.KATMED_I18N.values[code][
    window.KATMED_I18N.keys.indexOf(dynamicKey)];
  assert.equal(window.__trf(dynamicKey, "A/42"),
    translatedDynamic.replace("{0}", "A/42"), code);
  for (const value of window.KATMED_I18N.values[code])
    assert.equal(typeof value, "string", code);
}
window.__applyLocale("ar");
assert.equal(document.documentElement.dir, "rtl");
assert.equal(nodes[0].nodeValue.trim(), window.KATMED_I18N.values.ar[
  window.KATMED_I18N.keys.indexOf("Einstellungen")]);
assert.equal(window.__formatPreviewDate("20.10.2026 08:30 CET"), "20/10/2026 08:30 CET");
window.__applyLocale("ja");
assert.equal(document.documentElement.dir, "ltr");
assert.notEqual(nodes[0].nodeValue.trim(), "Settings");
window.__applyLocale("de");
assert.equal(nodes[0].nodeValue, " Einstellungen ");
assert.equal(nodes[1].nodeValue, "My own alarm text");

(async () => {
  /* Exercise the actual runtime Setup status producer, then change language
   * while the Settings page stays open. Static DOM translation alone passes. */
  setupReplies.push({configured: false, mode: 2, setup_open: true,
    revision: "synthetic-setup-r1"});
  await window.__loadRuntimeSetup();
  assert.match(element("runtime-setup-state").textContent, /Ersteinrichtung/);
  window.__applyLocale("fr");
  assert.equal(element("runtime-setup-state").textContent,
    "Configuration initiale : choisissez un groupe ou quittez la configuration hors ligne. Sans groupe sûr, le mesh reste désactivé.");
  assert.equal(element("runtime-setup-exit").hidden, false);
  const initialKey = "Ersteinrichtung; Gruppe wählen oder Einrichtung offline verlassen. Ohne sichere Gruppe bleibt Mesh aus.";
  for (const code of supported) {
    window.__applyLocale(code);
    assert.equal(element("runtime-setup-state").textContent,
      window.KATMED_I18N.values[code][window.KATMED_I18N.keys.indexOf(initialKey)], code);
  }
  /* Model the normal language-form flow after the existing config GET reports
   * an error: state.config remains null, the language PUT succeeds, and the
   * existing Wi-Fi reload then returns the product's handled HTTP 503 class.
   * Only the established routes are represented; no new endpoint is added. */
  window.__applyLocale("de");
  window.__resetPreview();
  assert.equal(element("due-preview").textContent,
    window.KATMED_I18N.values.de[window.KATMED_I18N.keys.indexOf("Vor dem Speichern Vorschau prüfen.")]);
  element("language-select").value = "ar";
  languageReply = "ar";
  await element("language-form").listeners.submit({
    preventDefault() {}, currentTarget: element("language-form")});
  assert.equal(document.documentElement.lang, "ar");
  assert.equal(element("due-preview").textContent,
    window.KATMED_I18N.values.ar[window.KATMED_I18N.keys.indexOf("Vor dem Speichern Vorschau prüfen.")]);
  assert.equal(nodes[1].nodeValue, "My own alarm text");

  setupReplies.push({configured: true, mode: 0, setup_open: true,
    revision: "synthetic-setup-r2"});
  await window.__loadRuntimeSetup();
  window.__applyLocale("fr");
  assert.equal(element("runtime-setup-state").textContent,
    "Configuré ; enregistrer les modifications.");

  /* Exercise the existing bootstrap and language-form path with the real
   * product error status for config GET and the existing Wi-Fi 503 response.
   * The synthetic config error leaves state.config null; no new endpoint is
   * modeled and the successful language PUT uses the existing response shape. */
  setupReplies.push({configured: true, mode: 0, setup_open: true,
    revision: "synthetic-setup-r3"});
  await assert.rejects(window.__applyBootstrap({setup_required: false,
    authenticated: true, device_name: "Synthetic device", csrf_token: "fixture"}),
  (error) => error.status === 503);
  assert.equal(window.__state.config, null);
  window.__applyLocale("de");
  window.__resetPreview();
  const previewKey = "Vor dem Speichern Vorschau prüfen.";
  assert.equal(element("due-preview").textContent,
    window.KATMED_I18N.values.de[window.KATMED_I18N.keys.indexOf(previewKey)]);
  element("due-time").value = "13:37";
  element("language-select").value = "ar";
  languageReply = "ar";
  await element("language-form").listeners.submit({
    preventDefault() {}, currentTarget: element("language-form")});
  assert.equal(document.documentElement.lang, "ar");
  assert.equal(element("due-preview").textContent,
    window.KATMED_I18N.values.ar[window.KATMED_I18N.keys.indexOf(previewKey)]);
  assert.equal(element("due-time").value, "13:37");
  assert.equal(nodes[1].nodeValue, "My own alarm text");
  for (const code of ["ja", "en"]) {
    element("language-select").value = code;
    languageReply = code;
    languagePutStatus = 200;
    await element("language-form").listeners.submit({
      preventDefault() {}, currentTarget: element("language-form")});
    assert.equal(document.documentElement.lang, code);
    assert.equal(element("due-preview").textContent,
      window.KATMED_I18N.values[code][window.KATMED_I18N.keys.indexOf(previewKey)], code);
  }

  /* A valid already loaded configuration is the control for the known Wi-Fi
   * 503 after language save: renderConfig re-localizes the dynamic fallback. */
  window.__renderConfig({revision: "synthetic-config-r1", medication_hours: "12",
    image_hours: "6", next_due_hhmm: "08:30", next_due_state: "active",
    restore_status: "none", clock_style: "digital_large", orientation: 0,
    rotation: "name_order", brightness: "70", night_enabled: "off",
    night_start: "22:00", night_end: "06:00",
    night_copy: {local_saved: false, active: false, finished: false,
      membership_known: false, expected: 0, acknowledged: 0, rejected: 0}});
  element("language-select").value = "ja";
  languageReply = "ja";
  await element("language-form").listeners.submit({
    preventDefault() {}, currentTarget: element("language-form")});
  assert.equal(document.documentElement.lang, "ja");
  assert.equal(element("due-preview").textContent,
    window.KATMED_I18N.values.ja[window.KATMED_I18N.keys.indexOf(previewKey)]);
  assert.equal(nodes[1].nodeValue, "My own alarm text");
  element("language-select").value = "en";
  languagePutStatus = 503;
  await element("language-form").listeners.submit({
    preventDefault() {}, currentTarget: element("language-form")});
  assert.equal(document.documentElement.lang, "ja", "failed language save restores prior locale");
  assert.equal(element("due-preview").textContent,
    window.KATMED_I18N.values.ja[window.KATMED_I18N.keys.indexOf(previewKey)]);
  assert.equal(element("due-time").value, "08:30", "stored configuration remains unchanged");
  assert.equal(nodes[1].nodeValue, "My own alarm text");
  console.log("All 12 Web locales, RTL, DOM text and live Setup state: OK");
})().catch((error) => { console.error(error); process.exitCode = 1; });
