"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    id, disabled: false, hidden: false, textContent: "", value: "",
    classList: { toggle() {} }, listeners: {}, dataset: {}, style: {},
    addEventListener(type, callback) { this.listeners[type] = callback; },
    querySelectorAll() { return []; }, reset() {}, replaceChildren() {}, append() {},
    focus() { document.activeElement = this; }, showModal() { this.open = true; }, close() { this.open = false; }
  });
  return elements.get(id);
}
const replies = [];
const calls = [];
const document = {
  getElementById: element,
  querySelectorAll() { return []; },
  createElement(tag) { return element(`created-${tag}-${Math.random()}`); }
};
const windowEvents = {};
const window = { scrollY: 130, scrollTo(_x, y) { this.scrollY = y; },
  addEventListener(type, callback) { windowEvents[type] = callback; } };
const history = { pushState(state) { this.state = state; }, replaceState(state) { this.state = state; } };
const location = { hostname: "device.local", protocol: "http:", hash: "", pathname: "/", search: "" };
const script = fs.readFileSync(path.join(__dirname,
  "../../firmware/components/katmed_web/ui/app.js"), "utf8");
vm.runInNewContext(script, {
  document, window, history, location,
  fetch: async (url, options) => {
    calls.push({url, options});
    const reply = replies.shift();
    assert.ok(reply, "unexpected request");
    await Promise.resolve();
    return { ok: reply.status < 400, status: reply.status,
      headers: { get: () => "application/json" }, json: async () => reply.body };
  }
});

async function click(id) {
  const button = element(id);
  const event = { currentTarget: button };
  const pending = button.listeners.click(event);
  assert.equal(button.disabled, true);
  event.currentTarget = null; // Browser clears currentTarget after dispatch.
  await pending;
  assert.equal(button.disabled, false);
}

async function invoke(id) {
  const button = element(id);
  return button.listeners.click({currentTarget: button});
}

async function submit(id) {
  const form = element(id);
  await form.listeners.submit({ currentTarget: form, preventDefault() {} });
}

(async () => {
  replies.push({ status: 401, body: { error: "denied" } });
  await click("cert-login");
  assert.match(element("notice").textContent, /Anmeldung fehlgeschlagen/);
  replies.push({ status: 200, body: {} }, { status: 200,
    body: { setup_required: true, authenticated: false, setup_nonce: "test" } });
  await click("logout");
  assert.equal(element("notice").textContent, "Abgemeldet.");
  element("setup-password").value = "synthetic-long-password";
  element("setup-confirm").value = "synthetic-long-password";
  replies.push({ status: 401, body: { error: "denied" } });
  await submit("setup-form");
  assert.match(element("notice").textContent, /Einmalcode ungültig/);
  element("cert-file").files = [{ size: 64,
    text: async () => "-----BEGIN CERTIFICATE-----\nsynthetic\n-----END CERTIFICATE-----" }];
  replies.push({ status: 401, body: { error: "denied" } });
  await submit("cert-form");
  assert.match(element("notice").textContent, /Zertifikatsfreigabe ist falsch/);
  replies.push({ status: 401, body: { error: "denied" } });
  await submit("cert-remove-form");
  assert.match(element("notice").textContent, /Entfernen der Zertifikatsfreigabe/);
  // Advanced owns the three interval fields and restores the parent scroll.
  await invoke("open-advanced");
  assert.equal(element("main-settings").hidden, true);
  assert.equal(element("advanced-view").hidden, false);
  window.scrollY = 0;
  await invoke("advanced-back");
  assert.equal(element("main-settings").hidden, false);
  assert.equal(window.scrollY, 130);
  await invoke("open-advanced");
  element("advanced-view").listeners.touchstart({touches: [{clientX: 8, clientY: 45}]});
  element("advanced-view").listeners.touchend({changedTouches: [{clientX: 100, clientY: 52}]});
  assert.equal(element("advanced-view").hidden, true);
  assert.equal(window.scrollY, 130);

  // First action only obtains an expiring confirmation and opens warning.
  replies.push({status: 200, body: {confirmation: "synthetic-one-shot"}});
  await invoke("factory-reset-start");
  assert.equal(element("factory-reset-dialog").open, true);
  assert.equal(document.activeElement, element("factory-reset-no"));
  assert.equal(calls.at(-1).url, "/api/factory-reset/prepare");
  assert.equal(calls.some((call) => call.url === "/api/factory-reset/confirm"), false);
  await invoke("factory-reset-no");
  assert.equal(element("factory-reset-dialog").open, false);
  assert.equal(calls.some((call) => call.url === "/api/factory-reset/confirm"), false);

  replies.push({status: 200, body: {confirmation: "synthetic-second"}});
  await invoke("factory-reset-start");
  replies.push({status: 202, body: {accepted: true}});
  await invoke("factory-reset-yes");
  assert.equal(calls.at(-1).url, "/api/factory-reset/confirm");
  assert.equal(calls.at(-1).options.headers["X-Katmed-CSRF"], undefined);
  assert.equal(replies.length, 0);
  console.log("async buttons, Advanced scroll restore and two-stage reset: OK");
})().catch((error) => { console.error(error); process.exitCode = 1; });
