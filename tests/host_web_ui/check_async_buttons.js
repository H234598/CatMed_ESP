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
    children: [],
    querySelector(selector) { return selector === 'option[value="auto"]' ?
      this.children.find(child => child.value === "auto") || null : null; },
    appendChild(child) { child.parent = this; this.children.push(child); },
    remove() { if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this); },
    querySelectorAll() { return []; }, reset() {}, replaceChildren() {}, append() {},
    focus() { document.activeElement = this; }, showModal() { this.open = true; }, close() { this.open = false; }
  });
  return elements.get(id);
}
const replies = [];
const calls = [];
const timers = [];
const document = {
  documentElement: { lang: "en", dir: "ltr" }, title: "Fixture",
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
vm.runInNewContext(script.replace("const state = {", "const state = window.__testState = {")
  .replace("function renderConfig(config) {", "window.__renderConfig=renderConfig; window.__validConfig=validConfig; window.__saveField=saveField; function renderConfig(config) {")
  .replace("function cancelResetOnLeave() {", "window.__testShow = show; function cancelResetOnLeave() {"), {
  document, window, history, location,
  setTimeout: (callback, delay) => { callback.delay = delay; timers.push(callback); },
  fetch: async (url, options) => {
    calls.push({url, options});
    const reply = replies.shift();
    assert.ok(reply, "unexpected request");
    await Promise.resolve();
    if (reply.hold) await reply.hold;
    if (reply.network) throw new TypeError("synthetic reboot disconnect");
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
  const config={revision:"synthetic-r1",medication_hours:"6",image_hours:"8",
    next_due_hhmm:null,next_due_state:"none",restore_status:"none",
    clock_style:"digital_large",orientation:0,rotation:"off",
    orientation_auto_supported:false,orientation_automatic:false,
    brightness:"100",night_enabled:"off",night_start:"22:00",night_end:"07:00",
    night_copy:{local_saved:false,active:false,finished:false,membership_known:false,expected:0,acknowledged:0,rejected:0}};
  assert.equal(window.__validConfig({...config,brightness:"0"}),false);
  assert.equal(window.__validConfig({...config,night_end:"22:00"}),false);
  window.__renderConfig(config);
  const beforeCopy=calls.length;
  const copied={...config,night_copy:{local_saved:true,active:false,finished:true,membership_known:true,expected:2,acknowledged:1,rejected:0}};
  replies.push({status:200,body:copied});
  await click("copy-night");
  assert.equal(calls.length,beforeCopy+1);
  assert.equal(JSON.parse(calls.at(-1).options.body).field,"copy_night");
  assert.equal(JSON.parse(calls.at(-1).options.body).value,"copy");
  assert.match(element("night-copy-status").textContent,/1\/2/);
  assert.match(element("night-copy-status").textContent,/1 offen/);
  window.__renderConfig(config);
  assert.equal(element("orientation").querySelector('option[value="auto"]'),null);
  assert.equal(window.__validConfig({...config,orientation_automatic:true}),false);
  const autoConfig={...config,orientation_auto_supported:true,orientation_automatic:true};
  window.__renderConfig(autoConfig);
  assert.equal(element("orientation").value,"auto");
  assert.ok(element("orientation").querySelector('option[value="auto"]'));
  window.__renderConfig({...autoConfig,orientation_automatic:false,orientation:90});
  assert.equal(element("orientation").value,"90");
  window.__renderConfig(autoConfig);
  const form=element("orientation-form");form.dataset.field="orientation";
  form.elements={namedItem:()=>element("orientation")};form.reportValidity=()=>true;
  // A transient sensor pose can change between commit and readback without
  // changing the accepted automatic policy or its revision.
  replies.push({status:200,body:autoConfig},{status:200,body:{...autoConfig,orientation:90}});
  await window.__saveField(form);
  assert.equal(JSON.parse(calls[calls.length-2].options.body).value,"auto");
  assert.equal(element("orientation").value,"auto");
  assert.match(element("save-status").textContent,/Gespeichert/);
  window.__renderConfig(config);
  assert.equal(element("orientation").querySelector('option[value="auto"]'),null);
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

  // Web may only open the physical-button tile, inspect status, or cancel.
  replies.push({status: 202, body: {display: "preparing"}},
    {status: 200, body: {display: "preparing"}});
  await invoke("factory-reset-start");
  assert.equal(element("factory-reset-dialog").open, true);
  assert.equal(document.activeElement, element("factory-reset-no"));
  assert.equal(calls.at(-1).url, "/api/factory-reset/status");
  assert.doesNotMatch(element("reset-status").textContent, /RESET-Taste/);
  replies.push({status: 200, body: {display: "press_reset"}});
  await timers.shift()();
  assert.match(element("reset-status").textContent, /RESET-Taste/);
  assert.equal(calls.some((call) => call.url === "/api/factory-reset/confirm"), false);
  replies.push({status: 200, body: {display: "idle"}});
  await invoke("factory-reset-no");
  assert.equal(element("factory-reset-dialog").open, false);
  const beforeStalePoll = calls.length;
  await timers.shift()();
  assert.equal(calls.length, beforeStalePoll);
  assert.equal(calls.some((call) => call.url === "/api/factory-reset/confirm"), false);

  replies.push({status: 200, body: {display: "press_reset"}});
  await invoke("factory-reset-status");
  assert.match(element("reset-status").textContent, /RESET-Taste/);
  replies.push({status: 202, body: {display: "preparing"}},
    {status: 200, body: {display: "preparing"}});
  await invoke("factory-reset-start");
  replies.push({status: 200, body: {display: "confirm_on_display"}});
  await timers.shift()();
  assert.match(element("reset-status").textContent, /neu Ja/);
  assert.ok(timers.length && timers[0].delay >= 3000);
  replies.push({status: 409, body: {error: "reset_already_confirmed"}});
  await invoke("factory-reset-no");
  assert.equal(element("factory-reset-dialog").open, true);
  assert.match(element("reset-status").textContent, /Löschung/);
  assert.doesNotMatch(element("reset-status").textContent, /Keine Daten gelöscht/);
  replies.push({status: 200, body: {display: "reset_committed"}});
  await timers.shift()();
  assert.match(element("reset-status").textContent, /Löschung/);
  assert.equal(calls.at(-1).url, "/api/factory-reset/status");
  element("factory-reset-dialog").close();
  replies.push({status: 202, body: {display: "preparing"}},
    {status: 200, body: {display: "unavailable"}});
  await invoke("factory-reset-start");
  assert.match(element("reset-status").textContent, /nicht geöffnet/);
  assert.doesNotMatch(element("reset-status").textContent, /RESET-Taste/);
  replies.push({status: 409, body: {error: "reset_not_prepared"}});
  await invoke("factory-reset-no");
  assert.match(element("reset-status").textContent, /Keine aktive Resetvorbereitung/);
  element("factory-reset-dialog").close();
  replies.push({status: 202, body: {display: "preparing"}}, {network: true});
  await invoke("factory-reset-start");
  assert.match(element("reset-status").textContent, /nicht erreichbar/);

  // Display cancels after Web has already shown the physical-button prompt.
  element("factory-reset-dialog").close();
  replies.push({status: 202, body: {display: "preparing"}},
    {status: 200, body: {display: "press_reset"}});
  await invoke("factory-reset-start");
  assert.match(element("reset-status").textContent, /RESET-Taste/);
  assert.ok(timers.length && timers[0].delay >= 3000);
  replies.push({status: 200, body: {display: "idle"}});
  await timers.shift()();
  assert.match(element("reset-status").textContent, /Keine aktive Resetvorbereitung/);
  assert.doesNotMatch(element("reset-status").textContent, /RESET-Taste/);
  assert.equal(timers.length, 0);

  // Leaving while Prepare is in flight must cancel before dialog opening;
  // a late accepted response must not resurrect the dialog.
  element("factory-reset-dialog").close();
  window.__testState.bootstrap = {csrf_token: "synthetic-csrf"};
  let releasePrepare;
  const hold = new Promise((resolve) => { releasePrepare = resolve; });
  replies.push({status: 202, body: {display: "preparing"}, hold},
    {status: 409, body: {error: "reset_not_prepared"}},
    {status: 200, body: {display: "idle"}});
  const cancelBefore = calls.filter((call) => call.url === "/api/factory-reset/cancel").length;
  const statusBefore = calls.filter((call) => call.url === "/api/factory-reset/status").length;
  const pendingPrepare = invoke("factory-reset-start");
  assert.equal(element("factory-reset-dialog").open, false);
  windowEvents.pagehide();
  const leaveCancel = calls.at(-1);
  assert.equal(leaveCancel.url, "/api/factory-reset/cancel");
  assert.equal(leaveCancel.options.keepalive, true);
  assert.equal(leaveCancel.options.headers["X-Katmed-CSRF"], "synthetic-csrf");
  releasePrepare();
  await pendingPrepare;
  assert.equal(element("factory-reset-dialog").open, false);
  assert.equal(calls.filter((call) => call.url === "/api/factory-reset/cancel").length,
    cancelBefore + 2);
  assert.equal(calls.filter((call) => call.url === "/api/factory-reset/status").length,
    statusBefore);

  // A session loss can clear bootstrap while Prepare is still in flight.
  window.__testState.bootstrap = {csrf_token: "synthetic-csrf"};
  let releaseViewPrepare;
  const viewHold = new Promise((resolve) => { releaseViewPrepare = resolve; });
  replies.push({status: 202, body: {display: "preparing"}, hold: viewHold},
    {status: 409, body: {error: "reset_not_prepared"}},
    {status: 200, body: {display: "idle"}});
  const viewCancelBefore = calls.filter((call) => call.url === "/api/factory-reset/cancel").length;
  const viewStatusBefore = calls.filter((call) => call.url === "/api/factory-reset/status").length;
  const pendingViewPrepare = invoke("factory-reset-start");
  window.__testState.bootstrap = null;
  window.__testShow("login-view");
  assert.equal(element("login-view").hidden, false);
  assert.equal(calls.at(-1).url, "/api/factory-reset/cancel");
  assert.equal(calls.at(-1).options.headers["X-Katmed-CSRF"], "synthetic-csrf");
  releaseViewPrepare();
  await pendingViewPrepare;
  assert.equal(element("factory-reset-dialog").open, false);
  assert.equal(calls.filter((call) => call.url === "/api/factory-reset/cancel").length,
    viewCancelBefore + 2);
  assert.equal(calls.filter((call) => call.url === "/api/factory-reset/status").length,
    viewStatusBefore);

  // A restored document with an open dialog must refresh its invalidated poll.
  window.__testState.bootstrap = {csrf_token: "synthetic-csrf"};
  element("factory-reset-dialog").showModal();
  replies.push({status: 200, body: {display: "press_reset"}});
  await invoke("factory-reset-status");
  assert.match(element("reset-status").textContent, /RESET-Taste/);
  replies.push({status: 200, body: {display: "idle"}},
    {status: 200, body: {display: "idle"}});
  windowEvents.pagehide();
  await windowEvents.pageshow();
  assert.match(element("reset-status").textContent, /Keine aktive Resetvorbereitung/);
  const afterRestore = calls.length;
  await timers.shift()();
  assert.equal(calls.length, afterRestore);
  element("factory-reset-dialog").close();
  assert.equal(calls.some((call) => call.url === "/api/factory-reset/confirm"), false);
  assert.equal(replies.length, 0);
  console.log("async buttons, Advanced scroll restore and physical reset web flow: OK");
})().catch((error) => { console.error(error); process.exitCode = 1; });
