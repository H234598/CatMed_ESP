"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const state = { bootstrap: null, config: null, preview: null };
  const views = ["setup-view", "login-view", "private-view"];
  const fields = ["medication_hours", "image_hours", "next_due_hhmm", "clock_style", "orientation", "rotation"];
  const controls = {
    medication_hours: "medication-hours", image_hours: "image-hours",
    next_due_hhmm: "due-time", clock_style: "clock-style",
    orientation: "orientation", rotation: "rotation"
  };

  function notice(message, error = false) {
    const node = $("notice");
    node.textContent = message;
    node.classList.toggle("error", error);
    node.hidden = !message;
  }

  function show(view) {
    for (const id of views) $(id).hidden = id !== view;
  }

  function renderDeviceName(name) {
    if (typeof name !== "string" || !name || !$("device-info")) return;
    $("device-info").textContent = `Gerät: ${name}`;
  }

  function setBusy(form, busy) {
    for (const button of form.querySelectorAll("button")) button.disabled = busy;
  }

  function errorText(error) {
    if (error instanceof TypeError) return "Das Gerät ist derzeit nicht erreichbar.";
    if (error.status === 401) return "Die Anmeldung ist abgelaufen. Bitte erneut anmelden.";
    if (error.status === 403) return "Zugriff abgelehnt. Bitte Seite neu laden und erneut anmelden.";
    if (error.status === 409) return "Die Daten haben sich geändert. Aktuelle Werte wurden neu geladen.";
    if (error.status === 413) return "Die Eingabe ist zu groß.";
    if (error.status === 422) return "Ungültige Eingabe. Bitte Wert und Format prüfen.";
    if (error.status === 429) return "Zu viele Versuche. Bitte später erneut versuchen.";
    if (error.status === 503) return "Speichern derzeit nicht verfügbar. Bitte später erneut versuchen.";
    return "Anfrage fehlgeschlagen. Bitte erneut versuchen.";
  }

  async function api(path, method = "GET", body = null) {
    const headers = { Accept: "application/json" };
    if (body !== null) headers["Content-Type"] = "application/json";
    if (method !== "GET" && state.bootstrap?.csrf_token)
      headers["X-Katmed-CSRF"] = state.bootstrap.csrf_token;
    const response = await fetch(path, {
      method, headers, credentials: "same-origin", cache: "no-store",
      redirect: "error", body: body === null ? undefined : JSON.stringify(body)
    });
    const contentType = response.headers.get("Content-Type") || "";
    const data = contentType.includes("application/json") ? await response.json() : null;
    if (!response.ok) {
      const error = new Error("HTTP request failed");
      error.status = response.status;
      error.code = data && typeof data.error === "string" ? data.error : "";
      throw error;
    }
    return data;
  }

  function resetPreview() {
    state.preview = null;
    $("save-due").disabled = true;
    $("due-preview").textContent = "Vor dem Speichern Vorschau prüfen.";
  }

  function validConfig(config) {
    if (!config || typeof config.revision !== "string" || !config.revision) return false;
    if (!fields.every((field) => Object.prototype.hasOwnProperty.call(config, field))) return false;
    if (typeof config.medication_hours !== "string" || typeof config.image_hours !== "string") return false;
    if (config.next_due_hhmm !== null && typeof config.next_due_hhmm !== "string") return false;
    if (!["none", "active", "paused_boot"].includes(config.next_due_state)) return false;
    if ((config.next_due_state === "none") !== (config.next_due_hhmm === null)) return false;
    if (!["none", "expired", "conflict"].includes(config.restore_status)) return false;
    if (!["digital_large", "digital_date", "analog"].includes(config.clock_style)) return false;
    if (![0, 90, 180, 270].includes(config.orientation)) return false;
    return ["off", "name_order", "random"].includes(config.rotation);
  }

  function renderConfig(config) {
    if (!validConfig(config)) throw new Error("Invalid configuration response");
    state.config = config;
    $(controls.medication_hours).value = config.medication_hours;
    $(controls.image_hours).value = config.image_hours;
    $(controls.next_due_hhmm).value = config.next_due_hhmm || "";
    $(controls.clock_style).value = config.clock_style;
    $(controls.orientation).value = String(config.orientation);
    $(controls.rotation).value = config.rotation;
    $("due-current").textContent = config.next_due_state === "none" ?
      "Derzeit kein Termin verfügbar." : config.next_due_state === "paused_boot" ?
      `Boot-Alarm aktiv; gespeicherter Termin ${config.next_due_hhmm} pausiert.` :
      `Aktuell: ${config.next_due_hhmm}`;
    const restore = config.restore_status === "expired" ?
      "Gespeicherter HH:MM-Termin ist abgelaufen." : config.restore_status === "conflict" ?
      "Gespeicherter HH:MM-Termin konnte nach Neustart nicht übernommen werden." : "";
    $("due-restore").textContent = restore;
    $("due-restore").hidden = !restore;
    resetPreview();
  }

  async function loadConfig() {
    renderConfig(await api("/api/config"));
  }

  function renderCertificate(certificate) {
    const enrolled = certificate && certificate.enrolled === true;
    $("cert-state").textContent = enrolled ?
      `Freigegeben${certificate.subject ? `: ${certificate.subject}` : "."}` :
      "Kein Clientzertifikat freigegeben.";
    $("cert-remove-form").hidden = !enrolled;
    $("cert-form").hidden = enrolled;
  }

  async function loadCertificate() {
    renderCertificate(await api("/api/client-cert"));
  }

  async function applyBootstrap(bootstrap) {
    if (!bootstrap || typeof bootstrap.setup_required !== "boolean" ||
        typeof bootstrap.authenticated !== "boolean")
      throw new Error("Invalid bootstrap response");
    state.bootstrap = bootstrap;
    state.config = null;
    resetPreview();
    renderDeviceName(bootstrap.device_name);
    if (bootstrap.setup_required) {
      show("setup-view");
      return;
    }
    if (!bootstrap.authenticated) {
      show("login-view");
      return;
    }
    show("private-view");
    $("save-status").textContent = "Aktuelle Werte werden geladen.";
    await Promise.all([loadConfig(), loadCertificate()]);
    $("save-status").textContent = "Aktuelle Werte geladen.";
  }

  async function refreshBootstrap() {
    await applyBootstrap(await api("/api/bootstrap"));
  }

  async function handleFailure(error, refresh = false) {
    if (error.status === 401) {
      state.bootstrap = null;
      state.config = null;
      show("login-view");
      try { await refreshBootstrap(); } catch (_) { /* Keep locked view. */ }
    } else if (refresh && error.status === 409) {
      try { await loadConfig(); } catch (_) { /* Keep prior form without claiming success. */ }
    }
    notice(errorText(error), true);
  }

  async function handleLoginFailure(error) {
    if (error.status === 401) {
      notice("Anmeldung fehlgeschlagen. Passwort oder Clientzertifikat prüfen.", true);
      return;
    }
    await handleFailure(error);
  }

  async function saveField(form) {
    if (!state.config) return;
    const field = form.dataset.field;
    if (!fields.includes(field)) return;
    const input = form.elements.namedItem("value");
    if (!input || !form.reportValidity()) return;
    const value = field === "orientation" ? Number(input.value) : input.value.trim();
    const request = { field, value, revision: state.config.revision };
    if (field === "next_due_hhmm") {
      if (!state.preview || state.preview.time !== value ||
          state.preview.revision !== state.config.revision) {
        notice("Bitte die Fälligkeit erneut prüfen.", true);
        return;
      }
      request.preview_token = state.preview.token;
    }
    setBusy(form, true);
    $("save-status").textContent = "Speichern läuft …";
    notice("");
    try {
      const committed = await api("/api/config", "PUT", request);
      const readback = await api("/api/config");
      if (!validConfig(committed) || !validConfig(readback) ||
          committed.revision !== readback.revision ||
          committed[field] !== readback[field])
        throw new Error("Readback differs from committed configuration");
      renderConfig(readback);
      $("save-status").textContent = "Gespeichert und bestätigt.";
      notice("Einstellung gespeichert.");
    } catch (error) {
      $("save-status").textContent = "Speichern nicht bestätigt.";
      await handleFailure(error, true);
    } finally {
      setBusy(form, false);
      if (field === "next_due_hhmm" && !state.preview) $("save-due").disabled = true;
    }
  }

  async function previewDue() {
    const form = $("due-form");
    if (!state.config || !form.reportValidity()) return;
    const time = $("due-time").value.trim();
    resetPreview();
    setBusy(form, true);
    notice("");
    try {
      const preview = await api("/api/config/preview-time", "POST", {
        time, revision: state.config.revision
      });
      if (!preview || preview.time !== time || typeof preview.preview_token !== "string" ||
          typeof preview.regular_local !== "string" ||
          typeof preview.effective_local !== "string")
        throw new Error("Invalid preview response");
      state.preview = { time, revision: state.config.revision, token: preview.preview_token };
      let description = `Termin: ${preview.regular_local}. Wirksam: ${preview.effective_local}.`;
      if (preview.skipped_nonexistent_time) description += " Diese Uhrzeit entfällt durch die Zeitumstellung.";
      if (preview.night_advanced) description += " Nachtvorziehung aktiv.";
      $("due-preview").textContent = description;
    } catch (error) {
      await handleFailure(error, true);
    } finally {
      setBusy(form, false);
      $("save-due").disabled = !state.preview;
    }
  }

  $("setup-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const password = $("setup-password").value;
    if (password !== $("setup-confirm").value) {
      notice("Die Passwörter stimmen nicht überein.", true);
      return;
    }
    setBusy(form, true);
    try {
      await api("/api/setup", "POST", {
        password, setup_nonce: state.bootstrap?.setup_nonce,
        setup_code: $("setup-code").value
      });
      form.reset();
      notice("Passwort eingerichtet. Bitte anmelden.");
      await refreshBootstrap();
    } catch (error) {
      if (error.status === 401 && error.code === "denied")
        notice("Einmalcode ungültig oder abgelaufen. Bitte Code am Display prüfen.", true);
      else await handleFailure(error);
    } finally { setBusy(form, false); }
  });

  $("login-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    setBusy(form, true);
    try {
      const result = await api("/api/login", "POST", {
        method: "password", password: $("login-password").value
      });
      form.reset();
      notice("");
      await applyBootstrap(result);
    } catch (error) { await handleLoginFailure(error); }
    finally { setBusy(form, false); }
  });

  $("cert-login").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    try {
      notice("");
      await applyBootstrap(await api("/api/login", "POST", { method: "certificate" }));
    } catch (error) { await handleLoginFailure(error); }
    finally { button.disabled = false; }
  });

  $("logout").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    try {
      await api("/api/logout", "POST", {});
      state.bootstrap = null;
      state.config = null;
      $("cert-form").reset();
      $("cert-remove-form").reset();
      await refreshBootstrap();
      notice("Abgemeldet.");
    } catch (error) { await handleFailure(error); }
    finally { button.disabled = false; }
  });

  for (const form of document.querySelectorAll("form[data-field]")) {
    form.addEventListener("submit", (event) => { event.preventDefault(); void saveField(form); });
  }
  $("due-time").addEventListener("input", resetPreview);
  $("preview-due").addEventListener("click", () => { void previewDue(); });

  $("cert-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const file = $("cert-file").files[0];
    if (!file || file.size > 8192) {
      notice("Bitte ein öffentliches PEM-Zertifikat bis 8 KiB auswählen.", true);
      return;
    }
    setBusy(form, true);
    try {
      const certificate_pem = await file.text();
      if (!certificate_pem.includes("-----BEGIN CERTIFICATE-----") ||
          certificate_pem.includes("PRIVATE KEY")) {
        notice("Die Datei muss ein öffentliches Clientzertifikat enthalten.", true);
        return;
      }
      await api("/api/client-cert", "POST", {
        certificate_pem, password: $("cert-password").value
      });
      form.reset();
      await loadCertificate();
      notice("Clientzertifikat freigegeben.");
    } catch (error) {
      if (error.status === 401 && error.code === "denied")
        notice("Passwort zur Zertifikatsfreigabe ist falsch.", true);
      else await handleFailure(error);
    }
    finally { setBusy(form, false); }
  });

  $("cert-remove-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    setBusy(form, true);
    try {
      await api("/api/client-cert", "DELETE", { password: $("cert-remove-password").value });
      form.reset();
      await loadCertificate();
      notice("Clientzertifikat-Freigabe entfernt.");
    } catch (error) {
      if (error.status === 401 && error.code === "denied")
        notice("Passwort zum Entfernen der Zertifikatsfreigabe ist falsch.", true);
      else await handleFailure(error);
    }
    finally { setBusy(form, false); }
  });

  const loopback = ["localhost", "127.0.0.1", "[::1]"].includes(location.hostname);
  if (location.protocol !== "https:" && !loopback) {
    $("connection").textContent = "HTTPS erforderlich";
    notice("Diese Konfiguration ist nur über HTTPS erreichbar.", true);
  } else {
    $("connection").textContent = loopback && location.protocol === "http:" ?
      "Lokaler Test" : "Sichere lokale Verbindung";
    refreshBootstrap().catch((error) => notice(errorText(error), true));
  }
})();
