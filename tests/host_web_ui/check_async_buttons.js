"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    id, disabled: false, hidden: false, textContent: "", value: "",
    classList: { toggle() {} }, listeners: {},
    addEventListener(type, callback) { this.listeners[type] = callback; },
    querySelectorAll() { return []; }, reset() {}
  });
  return elements.get(id);
}
const replies = [];
const document = {
  getElementById: element,
  querySelectorAll() { return []; }
};
const script = fs.readFileSync(path.join(__dirname,
  "../../firmware/components/katmed_web/ui/app.js"), "utf8");
vm.runInNewContext(script, {
  document,
  location: { hostname: "device.local", protocol: "http:" },
  fetch: async () => {
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
  assert.equal(replies.length, 0);
  console.log("async buttons and contextual 401 messages: OK");
})().catch((error) => { console.error(error); process.exitCode = 1; });
