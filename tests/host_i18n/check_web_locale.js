"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const ui = path.resolve(__dirname, "../../firmware/components/katmed_web/ui");
const html = fs.readFileSync(path.join(ui, "index.html"), "utf8");
for (const code of ["bar", "tlh-Latn"])
  assert.match(html, new RegExp(`<option value="${code}">`));
const nodes = [{ nodeValue: " Einstellungen " }, { nodeValue: "My own alarm text" }];
const fields = [{ attrs: { placeholder: "Passwort eingeben" },
  getAttribute(name) { return this.attrs[name] || null; },
  setAttribute(name, value) { this.attrs[name] = value; } }];
const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    id, value: "", dataset: {}, style: {}, classList: { toggle() {}, remove() {} },
    addEventListener() {}, querySelectorAll() { return []; },
    append() {}, replaceChildren() {}, reset() {}, close() {}, showModal() {},
  });
  return elements.get(id);
}
const document = {
  title: "KatMed · Einstellungen", documentElement: { lang: "de", dir: "ltr" },
  body: { classList: { remove() {} } },
  getElementById: element,
  createElement: () => element(`new-${elements.size}`),
  querySelectorAll: (selector) => selector === "[placeholder],[title],[aria-label]" ? fields : [],
  createTreeWalker() {
    let index = 0;
    return { nextNode: () => nodes[index++] || null };
  },
};
const setupReplies = [];
const window = { addEventListener() {}, scrollY: 0, scrollTo() {} };
const context = { window, document, NodeFilter: { SHOW_TEXT: 4 },
  history: { pushState() {}, replaceState() {} },
  location: { hostname: "localhost", protocol: "https:", hash: "", pathname: "/", search: "" },
  setTimeout() {}, fetch: async (url) => {
    assert.equal(url, "/api/runtime-setup");
    const reply = setupReplies.shift();
    assert.ok(reply, "unexpected request");
    return {ok: true, headers: {get: () => "application/json"}, json: async () => reply};
  } };
vm.runInNewContext(fs.readFileSync(path.join(ui, "i18n.js"), "utf8"), context);
const app = fs.readFileSync(path.join(ui, "app.js"), "utf8")
  .replace("function applyLocale(language) {", "window.__applyLocale = applyLocale; function applyLocale(language) {")
  .replace("async function loadRuntimeEinrichtung() {", "window.__loadRuntimeSetup = loadRuntimeEinrichtung; async function loadRuntimeEinrichtung() {")
  .replace("function trf(source, ...values) {", "window.__trf = trf; function trf(source, ...values) {")
  .replace("function formatPreviewDate(value) {", "window.__formatPreviewDate = formatPreviewDate; function formatPreviewDate(value) {")
  .replace("void start();", "");
vm.runInNewContext(app, context);
assert.equal(document.documentElement.lang, "en");
assert.equal(nodes[0].nodeValue.trim(), "Settings");
assert.equal(nodes[1].nodeValue, "My own alarm text");
assert.equal(window.__formatPreviewDate("20.10.2026 08:30 CEST"), "2026-10-20 08:30 CEST");
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
  setupReplies.push({configured: true, mode: 0, setup_open: true,
    revision: "synthetic-setup-r2"});
  await window.__loadRuntimeSetup();
  window.__applyLocale("fr");
  assert.equal(element("runtime-setup-state").textContent,
    "Configuré ; enregistrer les modifications.");
  console.log("All 12 Web locales, RTL, DOM text and live Setup state: OK");
})().catch((error) => { console.error(error); process.exitCode = 1; });
