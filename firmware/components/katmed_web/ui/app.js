"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const state = { bootstrap: null, config: null, preview: null, wifiProfiles: [],
    wifiLimit: 0, advancedScrollY: 0,
    catalogOffset: 0, catalogCount: 0 };
  const views = ["setup-view", "login-view", "private-view"];
  const fields = ["medication_hours", "image_hours", "next_due_hhmm", "clock_style", "orientation", "rotation"];
  const controls = {
    medication_hours: "medication-hours", image_hours: "image-hours",
    next_due_hhmm: "due-time", clock_style: "clock-style",
    orientation: "orientation", rotation: "rotation"
  };
  let advancedTouchStart = null;
  let resetPoll = 0;
  let resetPending = false, resetLeaving = false;

  function notice(message, error = false) {
    const node = $("notice");
    node.textContent = message;
    node.classList.toggle("error", error);
    node.hidden = !message;
  }

  function show(view) {
    if (view !== "private-view" && $("factory-reset-dialog")?.open) {
      ++resetPoll;
      api("/api/factory-reset/cancel", "POST", {}).catch(() => {});
      $("factory-reset-dialog").close();
    }
    for (const id of views) $(id).hidden = id !== view;
  }

  function cancelResetOnLeave() {
    if (!state.bootstrap?.csrf_token) return;
    fetch("/api/factory-reset/cancel", {
      method: "POST", credentials: "same-origin", keepalive: true,
      headers: {"X-Katmed-CSRF": state.bootstrap.csrf_token}
    }).catch(() => {});
  }

  window.addEventListener("pagehide", () => {
    if (!resetPending && !$("factory-reset-dialog")?.open) return;
    resetLeaving = true;
    ++resetPoll;
    cancelResetOnLeave();
  });
  window.addEventListener("pageshow", () => { if (!resetPending) resetLeaving = false; });

  function renderDeviceName(name) {
    if (typeof name !== "string" || !name || !$("device-info")) return;
    $("device-info").textContent = `Gerät: ${name}`;
  }

  function setBusy(form, busy) {
    for (const button of form.querySelectorAll("button")) button.disabled = busy;
  }

  function errorText(error) {
    if (error instanceof TypeError || error.name === "TypeError") return "Das Gerät ist derzeit nicht erreichbar.";
    if (error.status === 401) return "Die Anmeldung ist abgelaufen. Bitte erneut anmelden.";
    if (error.status === 403) return "Zugriff abgelehnt. Bitte Seite neu laden und erneut anmelden.";
    if (error.status === 409) return "Die Daten haben sich geändert. Aktuelle Werte wurden neu geladen.";
    if (error.status === 413) return "Die Eingabe ist zu groß.";
    if (error.status === 422) return "Ungültige Eingabe. Bitte Wert und Format prüfen.";
    if (error.status === 429) return "Zu viele Versuche. Bitte später erneut versuchen.";
    if (error.status === 503) return "Speichern derzeit nicht verfügbar. Bitte später erneut versuchen.";
    if (error.status === 507) return "Der Bildspeicher ist voll oder derzeit nicht verfügbar.";
    return "Anfrage fehlgeschlagen. Bitte erneut versuchen.";
  }

  async function api(path, method = "GET", body = null) {
    const headers = { Accept: "application/json" };
    const rawBody = typeof Blob !== "undefined" && body instanceof Blob;
    if (body !== null) headers["Content-Type"] = rawBody ? body.type : "application/json";
    if (method !== "GET" && state.bootstrap?.csrf_token)
      headers["X-Katmed-CSRF"] = state.bootstrap.csrf_token;
    const response = await fetch(path, {
      method, headers, credentials: "same-origin", cache: "no-store",
      redirect: "error", body: body === null ? undefined : rawBody ? body : JSON.stringify(body)
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

  async function loadWifi() {
    const wifi = await api("/api/wifi");
    if (!wifi || typeof wifi.configured !== "boolean" ||
        typeof wifi.recovery_available !== "boolean" ||
        !Number.isSafeInteger(wifi.profile_limit) || wifi.profile_limit < 0 ||
        !Array.isArray(wifi.profiles) || wifi.profiles.length > wifi.profile_limit ||
        wifi.profiles.some((p) => !p || !Number.isSafeInteger(p.id) ||
          typeof p.ssid !== "string" || !p.ssid || !Number.isSafeInteger(p.priority) ||
          p.priority < 1 || typeof p.secured !== "boolean"))
      throw new Error("Invalid WLAN status response");
    state.wifiProfiles = wifi.profiles.map((p) => ({...p}));
    state.wifiLimit = wifi.profile_limit;
    $("wifi-state").textContent = wifi.configured ?
      ("WLAN-Daten gespeichert. Wiederherstellungsweg " +
       (wifi.recovery_available ? "verfügbar." : "nicht verfügbar.")) :
      "Noch keine gespeicherten WLAN-Daten. Firmware-Standardwerte können weiter gelten.";
    $("wifi-limit").textContent = `${wifi.profiles.length} von ${wifi.profile_limit} WLAN-Profilen belegt. Priorität 1 ist die höchste.`;
    $("wifi-reconnect").disabled = wifi.profiles.length === 0;
    renderWifiProfiles();
  }

  function renderWifiProfiles() {
    const list = $("wifi-profile-list");
    list.replaceChildren();
    const ordered = state.wifiProfiles.map((profile, index) => ({profile, index}))
      .sort((a, b) => a.profile.priority - b.profile.priority || a.index - b.index);
    for (const {profile, index} of ordered) {
      const item = document.createElement("li");
      const summary = document.createElement("span");
      summary.textContent = `Priorität ${profile.priority} · ${profile.ssid} · ${profile.secured ? "Schlüssel gespeichert" : "offenes WLAN"}`;
      const actions = document.createElement("span");
      actions.className = "actions";
      const edit = document.createElement("button");
      edit.type = "button";
      edit.className = "secondary";
      edit.textContent = "Bearbeiten";
      edit.dataset.profileEdit = String(index);
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "danger";
      remove.textContent = "Entfernen";
      remove.dataset.profileDelete = String(profile.id);
      actions.append(edit, remove);
      item.append(summary, actions);
      list.append(item);
    }
    $("wifi-profile-save").textContent = $("wifi-profile-form").dataset.editId ?
      "Profiländerung speichern" : "Profil zur Liste hinzufügen";
    $("wifi-profile-save").disabled = state.wifiProfiles.length >= state.wifiLimit &&
      !$("wifi-profile-form").dataset.editId;
  }

  function openAdvanced() {
    state.advancedScrollY = window.scrollY;
    $("main-settings").hidden = true;
    $("advanced-view").hidden = false;
    window.scrollTo(0, 0);
    history.pushState({katmedAdvanced: true}, "", "#advanced");
    $("advanced-view").focus({preventScroll: true});
  }

  function closeAdvanced(restore = true) {
    $("advanced-view").hidden = true;
    $("main-settings").hidden = false;
    if (location.hash === "#advanced") history.replaceState({}, "", location.pathname + location.search);
    if (restore) window.scrollTo(0, state.advancedScrollY);
  }

  function renderImages(page, append = false) {
    if (!page || !Array.isArray(page.images) ||
        !Number.isSafeInteger(page.used_bytes) || !Number.isSafeInteger(page.free_bytes) ||
        !Number.isSafeInteger(page.image_count) || typeof page.next_offset !== "boolean" ||
        page.images.some((image) => !image || !Number.isSafeInteger(image.id) ||
          !Number.isSafeInteger(image.bytes) || !Number.isSafeInteger(image.width) ||
          !Number.isSafeInteger(image.height)))
      throw new Error("Invalid image list response");
    const list = $("image-list");
    if (!append) list.replaceChildren();
    $("media-usage").textContent =
      page.image_count + " Bilder · " + page.used_bytes + " von " +
      (page.used_bytes + page.free_bytes) + " Byte belegt";
    for (const image of page.images) {
      const item = document.createElement("li");
      const details = document.createElement("div");
      const preview = document.createElement("img");
      preview.className = "image-preview";
      preview.loading = "lazy";
      preview.alt = "Gespeichertes Bild";
      preview.referrerPolicy = "no-referrer";
      preview.src = "/api/images?id=" + encodeURIComponent(image.id);
      const description = document.createElement("span");
      description.className = "help";
      description.textContent = ((image.id >>> 31) ? "Katalogkopie · " : "Eigenes Bild · ") +
        "JPEG " + image.width + " × " + image.height +
        " · " + image.bytes + " Byte";
      details.append(preview, description);
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "danger";
      remove.textContent = "Entfernen";
      remove.dataset.imageId = String(image.id);
      item.append(details, remove);
      list.append(item);
    }
  }

  async function loadImages() {
    let offset = 0;
    let append = false;
    let page;
    do {
      page = await api("/api/images?offset=" + offset);
      renderImages(page, append);
      append = true;
      offset += page.images.length;
      if (page.images.length === 0) break;
    } while (page.next_offset);
  }

  function renderCatalog(page, append = false) {
    if (!page || !Array.isArray(page.images) || !Number.isSafeInteger(page.catalog_count) ||
        !Number.isSafeInteger(page.offset) ||
        page.images.some((image) => !image || !Number.isSafeInteger(image.id) ||
          !Array.isArray(image.variants) || image.variants.length < 1 ||
          image.variants.some((variant) => !variant || !Number.isSafeInteger(variant.profile) ||
            !Number.isSafeInteger(variant.bytes) || !Number.isSafeInteger(variant.width) ||
            !Number.isSafeInteger(variant.height))))
      throw new Error("Invalid catalog response");
    const list = $("catalog-image-list");
    if (!append) list.replaceChildren();
    state.catalogCount = page.catalog_count;
    for (const image of page.images) {
      const item = document.createElement("li");
      const details = document.createElement("span");
      details.className = "help";
      details.textContent = "GitHub-Katalogbild " + (image.id + 1);
      const variants = document.createElement("span");
      variants.className = "actions";
      for (const variant of image.variants) {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "secondary";
        button.textContent = "Kopie übernehmen · " + variant.width + " × " +
          variant.height;
        button.dataset.catalogId = String(image.id);
        button.dataset.catalogProfile = String(variant.profile);
        variants.append(button);
      }
      item.append(details, variants);
      list.append(item);
    }
    state.catalogOffset = page.offset + page.images.length;
    $("catalog-more").hidden = state.catalogOffset >= state.catalogCount;
  }

  async function loadCatalog(append = false) {
    const offset = append ? state.catalogOffset : 0;
    const page = await api("/api/catalog/images?offset=" + offset);
    renderCatalog(page, append);
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
    await Promise.all([loadConfig(), loadCertificate(), loadWifi(), loadImages(), loadCatalog()]);
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

  $("open-advanced").addEventListener("click", openAdvanced);
  $("advanced-back").addEventListener("click", () => closeAdvanced());
  window.addEventListener("popstate", (event) => {
    if (!event.state?.katmedAdvanced && !$("advanced-view").hidden) closeAdvanced();
  });
  window.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !$("advanced-view").hidden) closeAdvanced();
  });
  $("advanced-view").addEventListener("touchstart", (event) => {
    if (event.touches.length !== 1) { advancedTouchStart = null; return; }
    advancedTouchStart = {x: event.touches[0].clientX, y: event.touches[0].clientY};
  }, {passive: true});
  $("advanced-view").addEventListener("touchend", (event) => {
    if (!advancedTouchStart || event.changedTouches.length !== 1) return;
    const dx = event.changedTouches[0].clientX - advancedTouchStart.x;
    const dy = event.changedTouches[0].clientY - advancedTouchStart.y;
    advancedTouchStart = null;
    if (dx > 64 && Math.abs(dx) > Math.abs(dy) * 1.4) closeAdvanced();
  }, {passive: true});

  $("wifi-open").addEventListener("change", () => {
    const password = $("wifi-password");
    password.disabled = $("wifi-open").checked;
    password.required = false;
    if (password.disabled) password.value = "";
  });

  $("wifi-profile-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const ssid = $("wifi-ssid").value;
    const password = $("wifi-password").value;
    const isOpen = $("wifi-open").checked;
    const editId = form.dataset.editId ? Number(form.dataset.editId) : null;
    const previous = editId === null ? null : state.wifiProfiles.find((p) => p.id === editId);
    const encoder = new TextEncoder();
    const ssidBytes = encoder.encode(ssid);
    const passwordBytes = encoder.encode(password);
    if (!ssidBytes.length || ssidBytes.length > 32 || passwordBytes.length > 64 ||
        (!isOpen && !password.length && !previous?.secured) ||
        (!isOpen && passwordBytes.length > 0 && passwordBytes.length < 8) ||
        (passwordBytes.length === 64 && !/^[0-9a-fA-F]{64}$/.test(password))) {
      notice("SSID oder WLAN-Schlüssel hat eine ungültige Länge.", true);
      return;
    }
    if (!editId && state.wifiProfiles.length >= state.wifiLimit) {
      notice("Der vom Gerätespeicher erlaubte Profilplatz ist ausgeschöpft.", true);
      return;
    }
    const priority = Number($("wifi-priority").value);
    if (!Number.isSafeInteger(priority) || priority < 1) {
      notice("Die Priorität muss eine positive ganze Zahl sein.", true);
      return;
    }
    setBusy(form, true);
    try {
      let profiles = state.wifiProfiles.map((profile) => ({
        id: profile.id, ssid: profile.ssid, priority: profile.priority,
        credential: "keep", password: ""
      }));
      const candidate = {
        ...(editId ? {id: editId} : {}), ssid, priority,
        credential: isOpen ? "open" : password ? "replace" : "keep", password
      };
      if (editId) profiles = profiles.map((profile) => profile.id === editId ? candidate : profile);
      else profiles.push(candidate);
      await api("/api/wifi/profiles", "PUT", {profiles});
      form.reset();
      delete form.dataset.editId;
      $("wifi-password").disabled = false;
      $("wifi-priority").value = "1";
      await loadWifi();
      notice("WLAN-Profile dauerhaft gespeichert. Zum Anwenden die Prioritäten bewusst neu bewerten lassen.");
    } catch (error) { await handleFailure(error); }
    finally { setBusy(form, false); }
  });

  $("wifi-profile-list").addEventListener("click", async (event) => {
    const edit = event.target.closest("button[data-profile-edit]");
    if (edit) {
      const profile = state.wifiProfiles[Number(edit.dataset.profileEdit)];
      if (!profile) return;
      const form = $("wifi-profile-form");
      form.dataset.editId = String(profile.id);
      $("wifi-ssid").value = profile.ssid;
      $("wifi-password").value = "";
      $("wifi-password").disabled = !profile.secured;
      $("wifi-open").checked = !profile.secured;
      $("wifi-priority").value = String(profile.priority);
      $("wifi-profile-save").textContent = "Profiländerung speichern";
      $("wifi-ssid").focus();
      return;
    }
    const remove = event.target.closest("button[data-profile-delete]");
    if (!remove) return;
    remove.disabled = true;
    try {
      await api("/api/wifi/profiles?id=" + encodeURIComponent(remove.dataset.profileDelete), "DELETE");
      await loadWifi();
      notice("WLAN-Profil entfernt. Die aktive Verbindung wird dadurch nicht automatisch gewechselt.");
    } catch (error) { await handleFailure(error); }
    finally { remove.disabled = false; }
  });

  $("wifi-reconnect").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    try {
      await api("/api/wifi/reconnect", "POST", {});
      notice("Prioritäten werden bei der bewussten Neuverbindung neu bewertet. Die Geräteadresse kann sich ändern.");
    } catch (error) {
      await handleFailure(error);
      button.disabled = false;
    }
  });

  $("factory-reset-start").addEventListener("click", async () => {
    if (resetPending) return;
    const dialog = $("factory-reset-dialog");
    const status = $("reset-status");
    const poll = ++resetPoll;
    status.textContent = "Displaykachel öffnet.";
    resetPending = true;
    try {
      await api("/api/factory-reset/prepare", "POST", {});
    } catch (error) {
      resetPending = false;
      if (resetLeaving) { resetLeaving = false; return; }
      status.textContent = errorText(error);
      notice(errorText(error), true);
      return;
    }
    resetPending = false;
    if (resetLeaving) { cancelResetOnLeave(); resetLeaving = false; return; }
    if (poll !== resetPoll) return;
    dialog.showModal();
    $("factory-reset-no").focus();
    await refreshResetStatus(poll);
  });

  async function refreshResetStatus(poll = resetPoll) {
    if (poll !== resetPoll) return;
    try {
      const result = await api("/api/factory-reset/status");
      if (poll !== resetPoll) return;
      const display = result.display;
      $("reset-status").textContent = display === "press_reset" ?
        "Kachel offen: Physische RESET-Taste drücken." :
        display === "confirm_on_display" ? "Warnfrage: Am Display Nein oder neu Ja wählen." :
        display === "reset_committed" ? "Reset bestätigt; Löschung läuft oder steht an." :
        display === "preparing" ? "Displaykachel öffnet." :
        display === "idle" ? "Keine aktive Resetvorbereitung." :
        "Kachel nicht geöffnet. Display prüfen.";
      if (["preparing", "press_reset", "confirm_on_display"].includes(display) && $("factory-reset-dialog").open)
        setTimeout(() => refreshResetStatus(poll), display === "preparing" ? 500 : 5000);
    } catch (error) {
      if (poll === resetPoll) $("reset-status").textContent = errorText(error);
    }
  }

  $("factory-reset-no").addEventListener("click", async () => {
    try {
      await api("/api/factory-reset/cancel", "POST", {});
      ++resetPoll;
      $("factory-reset-dialog").close();
      $("reset-status").textContent = "Vorbereitung abgebrochen.";
    } catch (error) {
      $("reset-status").textContent = error.code === "reset_already_confirmed" ?
        "Reset bereits bestätigt; Löschung steht an oder läuft." :
        error.code === "reset_not_prepared" ? "Keine aktive Resetvorbereitung." : errorText(error);
    }
  });

  $("factory-reset-dialog").addEventListener("cancel", (event) => {
    event.preventDefault();
    $("factory-reset-no").click();
  });

  $("factory-reset-status").addEventListener("click", async () => {
    await refreshResetStatus();
  });

  $("image-upload-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const file = $("image-upload").files[0];
    if (!file || file.type !== "image/jpeg" || file.size === 0 || file.size > 131072) {
      notice("Bitte eine JPEG-Datei bis 128 KiB auswählen.", true);
      return;
    }
    setBusy(form, true);
    try {
      await api("/api/images", "POST", file);
      form.reset();
      await loadImages();
      notice("Bild geprüft und gespeichert.");
    } catch (error) { await handleFailure(error); }
    finally { setBusy(form, false); }
  });

  $("image-list").addEventListener("click", async (event) => {
    const button = event.target.closest("button[data-image-id]");
    if (!button) return;
    button.disabled = true;
    try {
      await api("/api/images?id=" + encodeURIComponent(button.dataset.imageId), "DELETE");
      await loadImages();
      notice("Bild entfernt.");
    } catch (error) { await handleFailure(error); }
    finally { button.disabled = false; }
  });

  $("catalog-image-list").addEventListener("click", async (event) => {
    const button = event.target.closest("button[data-catalog-id]");
    if (!button) return;
    button.disabled = true;
    try {
      const query = "?id=" + encodeURIComponent(button.dataset.catalogId) +
        "&profile=" + encodeURIComponent(button.dataset.catalogProfile);
      await api("/api/images/import" + query, "POST");
      await loadImages();
      notice("Katalogbild atomar in den beschreibbaren Speicher übernommen.");
    } catch (error) { await handleFailure(error); }
    finally { button.disabled = false; }
  });

  $("catalog-more").addEventListener("click", async () => {
    const button = $("catalog-more");
    button.disabled = true;
    try { await loadCatalog(true); }
    catch (error) { await handleFailure(error); }
    finally { button.disabled = false; }
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
