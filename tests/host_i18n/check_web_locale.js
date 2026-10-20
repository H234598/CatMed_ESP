"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const ui = path.resolve(__dirname, "../../firmware/components/katmed_web/ui");
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
const window = { addEventListener() {}, scrollY: 0, scrollTo() {} };
const context = { window, document, NodeFilter: { SHOW_TEXT: 4 },
  history: { pushState() {}, replaceState() {} },
  location: { hostname: "localhost", protocol: "https:", hash: "", pathname: "/", search: "" },
  setTimeout() {}, fetch: () => { throw new Error("unexpected network call"); } };
vm.runInNewContext(fs.readFileSync(path.join(ui, "i18n.js"), "utf8"), context);
const app = fs.readFileSync(path.join(ui, "app.js"), "utf8")
  .replace("function applyLocale(language) {", "window.__applyLocale = applyLocale; function applyLocale(language) {")
  .replace("function formatPreviewDate(value) {", "window.__formatPreviewDate = formatPreviewDate; function formatPreviewDate(value) {")
  .replace("void start();", "");
vm.runInNewContext(app, context);
assert.equal(document.documentElement.lang, "en");
assert.equal(nodes[0].nodeValue.trim(), "Settings");
assert.equal(nodes[1].nodeValue, "My own alarm text");
assert.equal(window.__formatPreviewDate("20.10.2026 08:30 CEST"), "2026-10-20 08:30 CEST");
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
console.log("Web locale, RTL, DOM text and user data: OK");
