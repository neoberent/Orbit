(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);

  const loginScreen = $("loginScreen");
  const dashboardScreen = $("dashboardScreen");
  const loginForm = $("loginForm");
  const loginButton = $("loginButton");
  const loginError = $("loginError");
  const notConfiguredNotice = $("notConfiguredNotice");
  const usernameField = $("username");
  const passwordField = $("password");
  const logoutButton = $("logoutButton");
  const connectionDot = $("connectionDot");
  const emptyState = $("emptyState");
  const downloadList = $("downloadList");
  const cardTemplate = $("cardTemplate");
  const filterScroll = $("filterScroll");

  const addFab = $("addFab");
  const addSheet = $("addSheet");
  const sheetClose = $("sheetClose");
  const cancelBtn = $("cancelBtn");
  const linkStep = $("linkStep");
  const filesStep = $("filesStep");
  const sourceField = $("sourceField");
  const sourceIcon = $("sourceIcon");
  const sourceClear = $("sourceClear");
  const sourceHint = $("sourceHint");
  const addError = $("addError");
  const uploadTile = $("uploadTile");
  const torrentFileInput = $("torrentFileInput");
  const connectionsRow = $("connectionsRow");
  const connMinus = $("connMinus");
  const connPlus = $("connPlus");
  const connValue = $("connValue");
  const backToLink = $("backToLink");
  const torrentName = $("torrentName");
  const torrentMeta = $("torrentMeta");
  const selectAllBtn = $("selectAllBtn");
  const selectNoneBtn = $("selectNoneBtn");
  const fileListBox = $("fileListBox");
  const filesError = $("filesError");
  const submitLinkBtn = $("submitLinkBtn");
  const submitTorrentBtn = $("submitTorrentBtn");
  const toast = $("toast");

  const ICONS = {
    link: '<svg viewBox="0 0 24 24" fill="none"><path d="M9 15l6-6M10 7l1-1a4 4 0 015.5 5.5l-1.5 1.5M14 17l-1 1A4 4 0 017.5 12.5L9 11" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg>',
    magnet: '<svg viewBox="0 0 24 24" fill="none"><path d="M7 4v7a5 5 0 0010 0V4" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/><path d="M7 4h3M14 4h3" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/></svg>',
    torrent: '<svg viewBox="0 0 24 24" fill="none"><path d="M4 9h16M8 4l2 5-2 5M16 4l-2 5 2 5M4 15h16" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>',
    http: '<svg viewBox="0 0 24 24" fill="none"><path d="M12 16V4m0 0L7 9m5-5l5 5" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/><path d="M4 15v3a2 2 0 002 2h12a2 2 0 002-2v-3" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg>',
    media: '<svg viewBox="0 0 24 24" fill="none"><rect x="3" y="5" width="18" height="14" rx="2" stroke="currentColor" stroke-width="1.6"/><path d="M10 9l6 3-6 3V9z" fill="currentColor"/></svg>',
    play: '<svg viewBox="0 0 24 24" fill="none"><path d="M7 4l13 8-13 8V4z" fill="currentColor"/></svg>',
    pause: '<svg viewBox="0 0 24 24" fill="none"><rect x="6" y="4" width="4" height="16" rx="1" fill="currentColor"/><rect x="14" y="4" width="4" height="16" rx="1" fill="currentColor"/></svg>',
    stop: '<svg viewBox="0 0 24 24" fill="none"><rect x="5" y="5" width="14" height="14" rx="2" fill="currentColor"/></svg>',
    retry: '<svg viewBox="0 0 24 24" fill="none"><path d="M4 12a8 8 0 1 1 2.5 5.8M4 12V6m0 6h6" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>',
    trash: '<svg viewBox="0 0 24 24" fill="none"><path d="M5 7h14M9 7V5a1 1 0 011-1h4a1 1 0 011 1v2m-9 0l1 12a2 2 0 002 2h6a2 2 0 002-2l1-12" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>',
    check: '<svg viewBox="0 0 24 24" fill="none"><path d="M5 13l4 4L19 7" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>',
  };

  let pollTimer = null;
  let pollInFlight = false;
  const cardsById = new Map();
  let latestItems = [];
  let activeFilter = "all";
  const POLL_INTERVAL_MS = 1500;

  // ---------- helpers ----------

  function showScreen(screen) {
    loginScreen.hidden = screen !== "login";
    dashboardScreen.hidden = screen !== "dashboard";
  }

  function formatBytes(bytes) {
    const units = ["B", "KB", "MB", "GB", "TB", "PB"];
    let value = Number(bytes) || 0;
    let i = 0;
    while (value >= 1024 && i + 1 < units.length) {
      value /= 1024;
      i++;
    }
    return i === 0 ? `${Math.round(value)} ${units[i]}` : `${value.toFixed(2)} ${units[i]}`;
  }

  function formatRate(bytesPerSec) {
    return `${formatBytes(Math.max(0, bytesPerSec))}/s`;
  }

  function formatEta(seconds) {
    if (seconds === null || seconds === undefined) return "--";
    let total = Math.max(0, Math.floor(seconds));
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    const pad = (n) => String(n).padStart(2, "0");
    return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
  }

  function stateClass(state) {
    return state.toLowerCase();
  }

  let toastTimer = null;
  function showToast(message, isError) {
    toast.textContent = message;
    toast.classList.toggle("error", !!isError);
    toast.hidden = false;
    requestAnimationFrame(() => toast.classList.add("show"));
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      toast.classList.remove("show");
      setTimeout(() => { toast.hidden = true; }, 200);
    }, 2600);
  }

  async function api(path, options = {}) {
    return fetch(path, { credentials: "same-origin", ...options });
  }

  async function apiJson(path, options = {}) {
    const res = await api(path, {
      ...options,
      headers: { "Content-Type": "application/json", ...(options.headers || {}) },
    });
    const data = await res.json().catch(() => ({}));
    return { ok: res.ok, status: res.status, data };
  }

  // ---------- auth ----------

  async function checkSession() {
    try {
      const res = await api("/api/session");
      const data = await res.json();
      if (!data.configured) {
        showScreen("login");
        notConfiguredNotice.hidden = false;
        loginForm.hidden = true;
        return;
      }
      notConfiguredNotice.hidden = true;
      loginForm.hidden = false;
      if (data.authenticated) {
        enterDashboard();
      } else {
        showScreen("login");
      }
    } catch (err) {
      showScreen("login");
    }
  }

  loginForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    loginError.hidden = true;
    loginButton.disabled = true;
    loginButton.textContent = "Signing In…";
    try {
      const res = await api("/api/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: usernameField.value, password: passwordField.value }),
      });
      const data = await res.json().catch(() => ({}));
      if (res.ok) {
        passwordField.value = "";
        enterDashboard();
      } else {
        loginError.textContent = data.error || "Sign in failed";
        loginError.hidden = false;
      }
    } catch (err) {
      loginError.textContent = "Could not reach Orbit";
      loginError.hidden = false;
    } finally {
      loginButton.disabled = false;
      loginButton.textContent = "Sign In";
    }
  });

  logoutButton.addEventListener("click", async () => {
    stopPolling();
    try {
      await api("/api/logout", { method: "POST" });
    } catch (err) {}
    showScreen("login");
  });

  function enterDashboard() {
    showScreen("dashboard");
    startPolling();
  }

  // ---------- polling / list ----------

  function startPolling() {
    stopPolling();
    pollOnce();
    pollTimer = setInterval(pollOnce, POLL_INTERVAL_MS);
  }

  function stopPolling() {
    if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
    connectionDot.classList.remove("live", "down");
    cardsById.clear();
    downloadList.innerHTML = "";
    latestItems = [];
  }

  async function pollOnce() {
    if (pollInFlight) return;
    pollInFlight = true;
    try {
      const res = await api("/api/downloads");
      if (res.status === 401) {
        stopPolling();
        showScreen("login");
        return;
      }
      if (!res.ok) throw new Error("poll failed");
      const data = await res.json();
      connectionDot.classList.remove("down");
      connectionDot.classList.add("live");
      latestItems = data.downloads || [];
      renderDownloads();
    } catch (err) {
      connectionDot.classList.remove("live");
      connectionDot.classList.add("down");
    } finally {
      pollInFlight = false;
    }
  }

  async function sendAction(id, action) {
    try {
      await api(`/api/downloads/${id}/${action}`, { method: "POST" });
      pollOnce();
    } catch (err) {}
  }

  function matchesFilter(item, filter) {
    switch (filter) {
      case "active": return item.isActive;
      case "completed": return item.state === "Completed";
      case "torrents": return item.type === "torrent";
      case "paused": return item.isPaused;
      default: return true;
    }
  }

  function iconButton(iconKey, cls, title, handler) {
    const btn = document.createElement("button");
    btn.className = `icon-action ${cls}`;
    btn.innerHTML = ICONS[iconKey];
    btn.title = title;
    btn.setAttribute("aria-label", title);
    btn.addEventListener("click", handler);
    return btn;
  }

  function buildActions(container, item) {
    container.innerHTML = "";

    if (item.isPaused) {
      container.appendChild(iconButton("play", "primary", "Resume", () => sendAction(item.id, "resume")));
    } else if (item.isActive) {
      container.appendChild(iconButton("pause", "", "Pause", () => sendAction(item.id, "pause")));
    } else if (item.state === "Error" || item.state === "Cancelled") {
      container.appendChild(iconButton("retry", "primary", "Retry", () => sendAction(item.id, "resume")));
    }

    if (item.state !== "Completed" && item.state !== "Cancelled" && item.state !== "Error") {
      container.appendChild(iconButton("stop", "danger", "Cancel", () => sendAction(item.id, "cancel")));
    } else {
      container.appendChild(iconButton("trash", "danger", "Remove", () => sendAction(item.id, "remove")));
    }
  }

  function renderDownloads() {
    const items = latestItems.filter((item) => matchesFilter(item, activeFilter));
    emptyState.hidden = latestItems.length > 0;

    if (latestItems.length > 0 && items.length === 0) {
      emptyState.hidden = false;
      emptyState.querySelector("p").textContent = "Nothing here";
      emptyState.querySelector(".empty-sub").textContent = "No downloads match this filter.";
    } else if (latestItems.length === 0) {
      emptyState.querySelector("p").textContent = "No downloads yet";
      emptyState.querySelector(".empty-sub").textContent = "Tap the + button to add a link, magnet, or torrent file.";
    }

    const seen = new Set();
    for (const item of items) {
      seen.add(item.id);
      let card = cardsById.get(item.id);
      if (!card) {
        card = cardTemplate.content.firstElementChild.cloneNode(true);
        card.dataset.id = String(item.id);
        downloadList.appendChild(card);
        cardsById.set(item.id, card);
      }
      updateCard(card, item);
    }

    for (const [id, card] of cardsById) {
      if (!seen.has(id)) {
        card.remove();
        cardsById.delete(id);
      }
    }

    // Only touch the DOM order when it actually differs - re-appending nodes that
    // are already in the right place restarts their CSS entrance animation and
    // looks like the whole list is refreshing.
    const desiredOrder = items.map((item) => String(item.id));
    const currentOrder = Array.from(downloadList.children).map((el) => el.dataset.id);
    const orderMatches =
      desiredOrder.length === currentOrder.length && desiredOrder.every((id, i) => id === currentOrder[i]);
    if (!orderMatches) {
      for (const item of items) {
        const card = cardsById.get(item.id);
        if (card) downloadList.appendChild(card);
      }
    }
  }

  function updateCard(card, item) {
    const state = item.state || "Unknown";
    const progressPct = Math.max(0, Math.min(1, item.progress || 0)) * 100;

    // Skip all DOM writes when nothing actually changed since the last poll -
    // rewriting className/textContent every tick (even to the same value) is
    // what made completed/idle downloads look like they kept refreshing.
    const signature = JSON.stringify([
      state, item.type, item.name, progressPct.toFixed(1), item.bytesDownloaded, item.totalBytes,
      item.downloadRate, item.eta, item.error, item.isPaused, item.isActive,
    ]);
    if (card.dataset.sig === signature) return;
    card.dataset.sig = signature;

    const typeIcon = card.querySelector(".type-icon");
    typeIcon.className = `type-icon ${item.type}`;
    typeIcon.innerHTML = ICONS[item.type] || ICONS.http;

    card.querySelector(".card-name").textContent = item.name || item.id;
    card.querySelector(".card-name").title = item.name || "";

    const badge = card.querySelector(".state-badge");
    badge.textContent = state;
    badge.className = `state-badge ${stateClass(state)}`;

    const fill = card.querySelector(".progress-fill");
    fill.style.width = `${progressPct}%`;
    fill.className = `progress-fill ${stateClass(state)}`;

    card.querySelector(".meta-progress").textContent = `${progressPct.toFixed(1)}%`;
    card.querySelector(".meta-size").textContent =
      item.totalBytes > 0 ? `${formatBytes(item.bytesDownloaded)} / ${formatBytes(item.totalBytes)}` : formatBytes(item.bytesDownloaded);
    card.querySelector(".meta-speed").textContent = item.isActive ? formatRate(item.downloadRate) : "";
    card.querySelector(".meta-eta").textContent = item.isActive && item.eta !== null ? `ETA ${formatEta(item.eta)}` : "";

    const errorEl = card.querySelector(".card-error");
    if (item.error) {
      errorEl.textContent = item.error;
      errorEl.hidden = false;
    } else {
      errorEl.hidden = true;
    }

    buildActions(card.querySelector(".card-actions"), item);
  }

  filterScroll.addEventListener("click", (e) => {
    const chip = e.target.closest(".chip");
    if (!chip) return;
    activeFilter = chip.dataset.filter;
    for (const el of filterScroll.querySelectorAll(".chip")) {
      const isActive = el === chip;
      el.classList.toggle("active", isActive);
      el.setAttribute("aria-selected", String(isActive));
    }
    renderDownloads();
  });

  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible") {
      if (!dashboardScreen.hidden && !pollTimer) startPolling();
    } else if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
  });

  // ---------- add download sheet ----------

  let connections = 8;
  let pendingToken = null;
  let pendingFiles = [];

  function isMagnet(text) {
    return text.toLowerCase().indexOf("magnet:?") === 0;
  }

  function sourceTypeIcon(text) {
    if (text.length === 0) return "link";
    if (isMagnet(text)) return "magnet";
    return "link";
  }

  function openSheet() {
    resetSheet();
    addSheet.hidden = false;
    requestAnimationFrame(() => addSheet.classList.add("open"));
    setTimeout(() => sourceField.focus({ preventScroll: true }), 260);
  }

  function closeSheet() {
    addSheet.classList.remove("open");
    setTimeout(() => { addSheet.hidden = true; }, 260);
  }

  function resetSheet() {
    sourceField.value = "";
    sourceClear.hidden = true;
    sourceIcon.innerHTML = ICONS.link;
    addError.hidden = true;
    filesError.hidden = true;
    connections = 8;
    connValue.textContent = "8";
    connectionsRow.hidden = false;
    pendingToken = null;
    pendingFiles = [];
    torrentFileInput.value = "";
    showLinkStep();
  }

  function showLinkStep() {
    linkStep.hidden = false;
    filesStep.hidden = true;
    submitLinkBtn.hidden = false;
    submitTorrentBtn.hidden = true;
  }

  function showFilesStep() {
    linkStep.hidden = true;
    filesStep.hidden = false;
    submitLinkBtn.hidden = true;
    submitTorrentBtn.hidden = false;
  }

  addFab.addEventListener("click", openSheet);
  sheetClose.addEventListener("click", closeSheet);
  cancelBtn.addEventListener("click", closeSheet);
  addSheet.addEventListener("click", (e) => { if (e.target === addSheet) closeSheet(); });
  backToLink.addEventListener("click", showLinkStep);

  sourceField.addEventListener("input", () => {
    addError.hidden = true;
    const text = sourceField.value.trim();
    sourceClear.hidden = text.length === 0;
    sourceIcon.innerHTML = ICONS[sourceTypeIcon(text)];
    connectionsRow.hidden = isMagnet(text);
  });

  sourceClear.addEventListener("click", () => {
    sourceField.value = "";
    sourceField.focus();
    sourceClear.hidden = true;
    sourceIcon.innerHTML = ICONS.link;
    connectionsRow.hidden = false;
  });

  connMinus.addEventListener("click", () => {
    connections = Math.max(1, connections - 1);
    connValue.textContent = String(connections);
  });
  connPlus.addEventListener("click", () => {
    connections = Math.min(32, connections + 1);
    connValue.textContent = String(connections);
  });

  torrentFileInput.addEventListener("change", async () => {
    const file = torrentFileInput.files && torrentFileInput.files[0];
    if (!file) return;
    addError.hidden = true;
    const originalLabel = uploadTile.querySelector(".upload-text strong").textContent;
    uploadTile.querySelector(".upload-text strong").textContent = "Reading torrent…";
    try {
      const buf = await file.arrayBuffer();
      const res = await api("/api/torrents/parse", {
        method: "POST",
        headers: { "Content-Type": "application/x-bittorrent" },
        body: buf,
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok || !data.ok) {
        addError.textContent = data.error || "Could not read that torrent file";
        addError.hidden = false;
        return;
      }
      pendingToken = data.token;
      pendingFiles = (data.files || []).map((f) => ({ ...f, checked: true }));
      torrentName.textContent = data.name || "Torrent";
      renderFileList();
      showFilesStep();
    } catch (err) {
      addError.textContent = "Could not upload the torrent file";
      addError.hidden = false;
    } finally {
      uploadTile.querySelector(".upload-text strong").textContent = originalLabel;
      torrentFileInput.value = "";
    }
  });

  function updateTorrentSummary() {
    const selected = pendingFiles.filter((f) => f.checked);
    const totalSize = selected.reduce((sum, f) => sum + f.size, 0);
    torrentMeta.textContent = `${selected.length} of ${pendingFiles.length} files · ${formatBytes(totalSize)}`;
  }

  function renderFileList() {
    fileListBox.innerHTML = "";
    for (const f of pendingFiles) {
      const row = document.createElement("div");
      row.className = `file-row ${f.checked ? "checked" : ""}`;
      row.innerHTML = `
        <span class="file-check">${ICONS.check}</span>
        <span class="file-path"></span>
        <span class="file-size">${formatBytes(f.size)}</span>
      `;
      row.querySelector(".file-path").textContent = f.path;
      row.addEventListener("click", () => {
        f.checked = !f.checked;
        row.classList.toggle("checked", f.checked);
        updateTorrentSummary();
      });
      fileListBox.appendChild(row);
    }
    updateTorrentSummary();
  }

  selectAllBtn.addEventListener("click", () => {
    for (const f of pendingFiles) f.checked = true;
    renderFileList();
  });
  selectNoneBtn.addEventListener("click", () => {
    for (const f of pendingFiles) f.checked = false;
    renderFileList();
  });

  submitLinkBtn.addEventListener("click", async () => {
    const source = sourceField.value.trim();
    if (source.length === 0) {
      addError.textContent = "Please enter a URL or magnet link.";
      addError.hidden = false;
      return;
    }
    submitLinkBtn.disabled = true;
    try {
      const { ok, data } = await apiJson("/api/downloads", {
        method: "POST",
        body: JSON.stringify({ source, maxConnections: connections }),
      });
      if (!ok) {
        addError.textContent = data.error || "Could not add that download";
        addError.hidden = false;
        return;
      }
      closeSheet();
      showToast("Download added");
      pollOnce();
    } catch (err) {
      addError.textContent = "Could not reach Orbit";
      addError.hidden = false;
    } finally {
      submitLinkBtn.disabled = false;
    }
  });

  submitTorrentBtn.addEventListener("click", async () => {
    const excluded = pendingFiles.filter((f) => !f.checked).map((f) => f.index);
    if (pendingFiles.length > 0 && excluded.length === pendingFiles.length) {
      filesError.textContent = "Select at least one file to download.";
      filesError.hidden = false;
      return;
    }
    filesError.hidden = true;
    submitTorrentBtn.disabled = true;
    try {
      const { ok, data } = await apiJson("/api/downloads/torrent", {
        method: "POST",
        body: JSON.stringify({ token: pendingToken, excludedFileIndices: excluded }),
      });
      if (!ok) {
        filesError.textContent = data.error || "Could not add that torrent";
        filesError.hidden = false;
        return;
      }
      closeSheet();
      showToast("Torrent added");
      pollOnce();
    } catch (err) {
      filesError.textContent = "Could not reach Orbit";
      filesError.hidden = false;
    } finally {
      submitTorrentBtn.disabled = false;
    }
  });

  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && !addSheet.hidden) closeSheet();
  });

  checkSession();
})();
