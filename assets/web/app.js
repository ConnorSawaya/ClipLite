/* ClipLite web UI. Native host injects JSON via window.__onNative(json).
   Page -> native via chrome.webview.postMessage({cmd,...}). */
"use strict";

const $ = (id) => document.getElementById(id);
const grid = $("grid");
const empty = $("empty");
const overlay = $("player-overlay");
const video = $("video");
const toastEl = $("toast");

let clips = [];
let currentId = null;
let toastTimer = 0;
let pcOn = true;
let micOn = true;
let clipHotkey = "F8";
let appView = "library";
let selectedGame = "";
let libraryView = "grid";
let captureStatus = null;
let captureSettings = null;
let sourceReady = false;
const dialogStack = [];
const backgroundInert = new Map();

function focusableIn(root) {
  return [...root.querySelectorAll('button, input, select, textarea, a[href], video[controls], [tabindex], [contenteditable="true"]')]
    .filter((el) => el.tabIndex >= 0 && !el.matches(":disabled") &&
      !el.closest("[inert]") && el.getClientRects().length > 0);
}

function focusVisible(el) {
  if (!el || el === document.body || el === document.documentElement ||
      !el.isConnected || el.closest("[inert]") || !el.getClientRects().length) return false;
  el.focus({ preventScroll: true });
  return document.activeElement === el;
}

function clipCard(id) {
  return [...grid.querySelectorAll(".card")].find((card) => card.dataset.id === id);
}

function focusAppFallback() {
  return [grid.querySelector(".card"), $("search"), $("nav-" + appView), $("btn-clipnow")]
    .some((el) => focusVisible(el));
}

function currentDialog() {
  return dialogStack[dialogStack.length - 1];
}

function syncDialogBackground() {
  const active = currentDialog();
  for (const el of document.body.children) {
    if (["SCRIPT", "STYLE", "LINK"].includes(el.tagName) || el === toastEl) continue;
    if (active) {
      if (!backgroundInert.has(el)) backgroundInert.set(el, el.inert);
      el.inert = el !== active.overlay;
    } else if (backgroundInert.has(el)) {
      el.inert = backgroundInert.get(el);
    }
  }
  if (!active) backgroundInert.clear();
}

function showDialog(dialogOverlay, preferredFocus, replaces) {
  closeMenus();
  let entry = dialogStack.find((item) => item.overlay === dialogOverlay);
  if (!entry) {
    const active = document.activeElement;
    const activeCard = active.closest && active.closest(".card");
    const previous = replaces && dialogStack.find((item) => item.overlay === replaces);
    entry = {
      overlay: dialogOverlay,
      root: dialogOverlay.querySelector('[role="dialog"]') || dialogOverlay.firstElementChild,
      returnFocus: previous ? previous.returnFocus : active,
      returnClip: previous ? previous.returnClip :
        (activeCard ? activeCard.dataset.id : currentId)
    };
    if (replaces) hideDialog(replaces, false);
    dialogStack.push(entry);
  }
  dialogOverlay.classList.remove("hidden");
  syncDialogBackground();
  if (currentDialog() !== entry) return;
  if (!focusVisible(preferredFocus) && !focusVisible(focusableIn(entry.root)[0])) {
    entry.root.tabIndex = -1;
    focusVisible(entry.root);
  }
}

function hideDialog(dialogOverlay, restoreFocus = true) {
  const index = dialogStack.findIndex((item) => item.overlay === dialogOverlay);
  const entry = index >= 0 ? dialogStack.splice(index, 1)[0] : null;
  dialogOverlay.classList.add("hidden");
  syncDialogBackground();
  if (!restoreFocus || !entry) return;
  const active = currentDialog();
  if (active) {
    if (active.root.contains(entry.returnFocus) && focusVisible(entry.returnFocus)) return;
    if (!focusVisible(focusableIn(active.root)[0])) {
      active.root.tabIndex = -1;
      focusVisible(active.root);
    }
  } else if (!focusVisible(entry.returnFocus) && !focusVisible(clipCard(entry.returnClip))) {
    focusAppFallback();
  }
}

document.addEventListener("keydown", (event) => {
  const active = currentDialog();
  if (!active || event.defaultPrevented || event.key !== "Tab") return;
  const controls = focusableIn(active.root);
  const index = controls.indexOf(document.activeElement);
  if (!controls.length) {
    event.preventDefault();
    active.root.tabIndex = -1;
    focusVisible(active.root);
  } else if (index < 0 || (event.shiftKey && index === 0) ||
      (!event.shiftKey && index === controls.length - 1)) {
    event.preventDefault();
    focusVisible(controls[event.shiftKey ? controls.length - 1 : 0]);
  }
});

document.addEventListener("focusin", (event) => {
  const active = currentDialog();
  if (active && !active.root.contains(event.target)) {
    if (!focusVisible(focusableIn(active.root)[0])) {
      active.root.tabIndex = -1;
      focusVisible(active.root);
    }
  }
});

function post(msg) {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(JSON.stringify(msg));
  }
}

function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  }[c]));
}

function fmtSize(bytes) {
  if (!bytes) return "";
  const mb = bytes / (1024 * 1024);
  return mb >= 100 ? Math.round(mb) + " MB" : mb.toFixed(1) + " MB";
}

function clipGame(clip) {
  return typeof clip.game === "string" ? clip.game.trim() : "";
}

function clipTitle(clip) {
  return clip.game || clip.file || clip.id || "Clip";
}

function updateGameSelection() {
  const filters = $("game-filters");
  if (!filters) return;
  filters.querySelectorAll("button[data-game]").forEach((button) => {
    const selected = button.dataset.game === selectedGame;
    button.setAttribute("aria-pressed", String(selected));
    button.classList.toggle("active", selected);
  });
}

function renderGameFilters() {
  const filters = $("game-filters");
  if (!filters) return;
  const groups = new Map();
  for (const clip of clips) {
    const game = clipGame(clip);
    if (game) groups.set(game, (groups.get(game) || 0) + 1);
  }
  if (selectedGame && !groups.has(selectedGame)) selectedGame = "";
  const focused = filters.contains(document.activeElement) ? document.activeElement.dataset.game : null;
  filters.innerHTML = [...groups.entries()]
    .sort(([a], [b]) => a.localeCompare(b, undefined, { sensitivity: "base", numeric: true }))
    .map(([game, count]) => `<button type="button" class="game-filter" data-game="${esc(game)}" aria-pressed="${game === selectedGame}" aria-label="${esc(game + ", " + count + (count === 1 ? " clip" : " clips"))}" title="${esc(game)}"><span class="game-name">${esc(game)}</span><span class="game-count" aria-hidden="true">${count.toLocaleString()}</span></button>`)
    .join("");
  if (!groups.size) filters.innerHTML = '<span class="game-filter-empty">Game groups appear as clips are saved.</span>';
  updateGameSelection();
  if (focused) {
    const replacement = [...filters.querySelectorAll("button[data-game]")]
      .find((button) => button.dataset.game === focused);
    if (!focusVisible(replacement)) focusVisible($("nav-library"));
  }
}

function showAppView(view) {
  const next = view === "capture" ? "capture" : "library";
  const changed = next !== appView;
  appView = next;
  document.body.dataset.appView = appView;
  closeMenus();
  for (const mode of ["library", "capture"]) {
    const page = $(mode + "-view");
    const button = $("nav-" + mode);
    if (page) {
      page.classList.toggle("hidden", mode !== appView);
      page.hidden = mode !== appView;
    }
    if (button) button.setAttribute("aria-pressed", String(mode === appView));
  }
  if (changed && appView === "capture") {
    post({ cmd: "get_settings" });
    post({ cmd: "list_sources" });
  }
}

function setLibraryView(view) {
  libraryView = view === "list" ? "list" : "grid";
  grid.classList.toggle("list-view", libraryView === "list");
  for (const mode of ["grid", "list"]) {
    const button = $("view-" + mode);
    if (button) button.setAttribute("aria-pressed", String(mode === libraryView));
  }
}

function updateCaptureSummary() {
  const state = $("capture-state");
  if (state) {
    const known = captureStatus && typeof captureStatus.recording === "boolean";
    state.textContent = known ? (captureStatus.recording ? "Recording" : "Idle") : "Loading status…";
    state.dataset.state = known ? (captureStatus.recording ? "live" : "idle") : "unknown";
  }
  for (const [id, field, unit] of [
    ["capture-replay", "replay", "s"],
    ["capture-fps", "fps", "fps"],
    ["capture-bitrate", "bitrate", "Mbps"]
  ]) {
    const element = $(id);
    const value = captureSettings && captureSettings[field];
    if (element) element.textContent = typeof value === "number" && Number.isFinite(value) && value > 0
      ? value.toLocaleString() + " " + unit : "—";
  }
  const source = $("capture-source-summary");
  if (source) {
    let summary = "Loading source…";
    if (sourceReady) {
      if (srcMode === "window") {
        summary = "Window · " + (srcWinTitle || srcWinExe || "No window selected");
        if (srcWinTitle && srcWinExe) summary += " · " + srcWinExe;
      } else {
        const display = displays.find((item) => item.i === activeDisp);
        summary = "Entire desktop · " + (display ? display.label : "Display " + (activeDisp + 1));
      }
    }
    source.textContent = summary;
    source.title = summary;
  }
}

function selectSettingsPage(key, moveFocus = false) {
  for (const name of ["recording", "audio", "capture", "general"]) {
    const active = name === key;
    const tab = $("set-nav-" + name);
    const pane = $("settings-" + name);
    tab.setAttribute("aria-selected", String(active));
    tab.tabIndex = active ? 0 : -1;
    pane.classList.toggle("hidden", !active);
    pane.hidden = !active;
    pane.inert = !active;
  }
  $("settings-overlay").querySelector(".settings-panels").scrollTop = 0;
  if (moveFocus) focusVisible($("set-nav-" + key));
}

function playerMediaState(mode, title = "", detail = "", retry = false) {
  const state = $("pv-media-state");
  state.classList.toggle("hidden", mode === "ready");
  state.dataset.mode = mode;
  $("pv-media-title").textContent = title;
  $("pv-media-detail").textContent = detail;
  $("pv-retry").classList.toggle("hidden", !retry);
  $("pv-retry").textContent = mode === "paused" ? "Play clip" : "Retry playback";
}

function handlePlayerPlayFailure(error) {
  if (overlay.classList.contains("hidden") || error.name === "AbortError") return;
  if (error.name === "NotAllowedError") {
    playerMediaState("paused", "Playback paused", "Choose Play clip to start playback.", true);
  } else {
    playerMediaState("error", "Unable to play this clip", "Try again, or open the original file from More → Open folder.", true);
  }
}

function updateLibrarySummary(visible, filtered) {
  const count = $("library-count");
  if (count) count.textContent = filtered
    ? `${visible.length.toLocaleString()} of ${clips.length.toLocaleString()} clips`
    : `${clips.length.toLocaleString()} clip${clips.length === 1 ? "" : "s"}`;
  const storage = $("library-storage");
  if (storage) {
    const known = clips.length > 0 && clips.every((c) =>
      typeof c.size === "number" && Number.isFinite(c.size) && c.size >= 0);
    storage.classList.toggle("hidden", !known);
    if (known) {
      let bytes = clips.reduce((sum, c) => sum + c.size, 0);
      const units = ["B", "KB", "MB", "GB", "TB"];
      let unit = 0;
      while (bytes >= 1024 && unit < units.length - 1) { bytes /= 1024; unit++; }
      storage.textContent = bytes.toLocaleString(undefined, {
        maximumFractionDigits: unit === 0 || bytes >= 100 ? 0 : 1
      }) + " " + units[unit] + " on disk";
    } else storage.textContent = "";
  }
}

function updateEmptyHotkey() {
  if ($("search").value.trim() || selectedGame) return;
  $("empty-sub").innerHTML = clipHotkey
    ? `Press <kbd>${esc(clipHotkey)}</kbd> in game to save your first replay.`
    : "Choose Clip Now to save your first replay.";
}

function render() {
  const q = ($("search").value || "").trim().toLowerCase();
  const visible = clips.filter((c) => (!selectedGame || clipGame(c) === selectedGame) &&
    (!q || ((c.game || "") + " " + (c.file || c.id)).toLowerCase().includes(q)));
  const sort = $("library-sort") ? $("library-sort").value : "newest";
  if (sort === "oldest") visible.reverse();
  else if (sort === "name") visible.sort((a, b) =>
    clipTitle(a).localeCompare(clipTitle(b), undefined, { sensitivity: "base", numeric: true }) ||
    (a.file || a.id || "").localeCompare(b.file || b.id || "", undefined, { sensitivity: "base", numeric: true }));

  const focusedCard = document.activeElement.closest && document.activeElement.closest(".card");
  const focusedId = focusedCard && focusedCard.dataset.id;
  grid.innerHTML = visible.map((c) => `
    <button type="button" class="card" data-id="${esc(c.id)}" title="${esc(c.file || c.id || c.game || "Clip")}" aria-label="${esc("Play " + (c.game || c.file || c.id || "clip") + (c.dur ? ", " + c.dur : ""))}">
      <span class="thumbwrap">
        ${c.thumb
          ? `<img src="${esc(c.thumb)}" alt="" draggable="false" loading="lazy" decoding="async">`
          : `<span class="thumb-ph"><svg viewBox="0 0 24 24" width="34" height="34" aria-hidden="true"><path fill="currentColor" d="M8 5v14l11-7z"/></svg></span>`}
        <span class="badge">${esc(c.dur || "--:--")}</span>
      </span>
      <span class="card-meta">
        <span class="card-title">${esc(c.game || c.file || c.id || "Clip")}</span>
        <span class="card-sub">${esc(c.date)}${c.size ? " · " + fmtSize(c.size) : ""}</span>
      </span>
    </button>`).join("");
  if (focusedId && !focusVisible(clipCard(focusedId))) {
    focusAppFallback();
  }
  updateLibrarySummary(visible, !!q || !!selectedGame);

  empty.classList.toggle("hidden", visible.length !== 0);
  empty.classList.toggle("show", visible.length === 0);
  if (visible.length === 0) {
    if (q) {
      $("empty-title").textContent = "No matches";
      $("empty-sub").textContent = selectedGame
        ? "Try a different search or choose Library." : "Try a different search.";
    } else if (selectedGame) {
      $("empty-title").textContent = "No clips for " + selectedGame;
      $("empty-sub").textContent = "Choose Library to see the rest of your clips.";
    } else {
      $("empty-title").textContent = "No clips yet";
      updateEmptyHotkey();
    }
  }
}

// Native -> page entry point (JSON string injected by ExecuteScript).
window.__onNative = function (raw) {
  let m;
  try { m = typeof raw === "string" ? JSON.parse(raw) : raw; } catch (e) { return; }
  handle(m);
};

function handle(m) {
  if (!m || !m.type) return;
  switch (m.type) {
    case "clips":
      clips = m.clips || [];
      renderGameFilters();
      render();
      break;
    case "status":
      setStatus(m);
      break;
    case "thumb": {
      const c = clips.find((x) => x.id === m.id);
      if (c && m.thumb) {
        c.thumb = m.thumb;
        const card = clipCard(c.id);
        if (card) {
          const thumb = card.querySelector(".thumbwrap");
          let img = thumb.querySelector("img");
          if (!img) {
            img = document.createElement("img");
            img.alt = "";
            img.draggable = false;
            img.loading = "lazy";
            img.decoding = "async";
            thumb.prepend(img);
          }
          img.src = m.thumb;
          const placeholder = thumb.querySelector(".thumb-ph");
          if (placeholder) placeholder.remove();
        }
      }
      break;
    }
    case "play_url": {
      const c = clips.find((x) => x.id === m.id);
      $("pv-game").textContent = c ? (c.game || c.file || c.id || "Clip") : "Clip";
      $("pv-sub").textContent = c ? `${c.date || ""} · ${c.dur || ""}` : "";
      video.src = m.url;
      showDialog(overlay);
      video.play().catch(handlePlayerPlayFailure);
      break;
    }
    case "toast":
      showToast(m.text || "");
      break;
    case "close_player":
      closePlayer();
      break;
    case "settings":
      captureSettings = m;
      updateCaptureSummary();
      $("s-replay").value = m.replay;
      $("s-fps").value = m.fps;
      $("s-bitrate").value = m.bitrate;
      $("s-quality").value = m.quality || "balanced";
      $("s-desktop").checked = !!m.desktop;
      $("s-mic").checked = !!m.mic;
      $("s-hotkey").value = m.hotkey || "(none)";
      $("s-startup").checked = !!m.startup;
      $("s-notify").checked = !!m.notifications;
      $("s-idlecap").checked = !!m.idlecap;
      $("s-gamesmaster").checked = !!m.gamesmaster;
      const gl = $("s-gamelist");
      gl.innerHTML = "";
      (m.games || []).forEach((g, i) => {
        if (!g.n) return;
        const row = document.createElement("div");
        row.className = "set-row";
        row.innerHTML = `<label for="s-game-${i}">${esc(g.n)}</label><input id="s-game-${i}" type="checkbox" data-game="${esc(g.n)}">`;
        row.querySelector("input").checked = !!g.c;
        gl.appendChild(row);
      });
      if (!(m.games || []).length) {
        gl.innerHTML =
          '<div class="blur-chip">No games seen yet — they appear here after you launch one.</div>';
      }
      break;
    case "displays":
      displays = m.list || [];
      activeDisp = m.active;
      renderSrcMenu();
      updateSrcLabel();
      break;
    case "sources":
      sourceReady = true;
      srcMode = m.mode === "window" ? "window" : "display";
      srcWinExe = m.window_exe || "";
      srcWinTitle = m.window_title || "";
      displays = m.displays || [];
      activeDisp = typeof m.display === "number" ? m.display : 0;
      srcWindows = m.windows || [];
      if (!srcMenu.classList.contains("hidden")) renderSrcMenu();
      updateSrcLabel();
      break;
    case "mics":
      micList = m.list || [];
      activeMicId = m.active || "";
      renderMics();
      break;
    case "miclevels": {
      const bars = {};
      (m.levels || []).forEach((l) => { bars[l.i] = l.p || 0; });
      [...micPop.querySelectorAll(".meter i")].forEach((el) => {
        const idAttr = el.getAttribute("data-id");
        if (bars[idAttr] !== undefined) el.style.width = bars[idAttr] + "%";
      });
      break;
    }
    case "open_settings":
      openSettings();
      break;
  }
}

function setStatus(st) {
  captureStatus = st;
  updateCaptureSummary();
  const chip = $("status-chip");
  const live = !!st.recording;
  chip.className = "chip " + (live ? "live" : "idle");
  $("status-text").textContent = live ? "Recording · " + (st.game || "Desktop") : "Idle";
  document.title = live ? "ClipLite — recording" : "ClipLite";
  if (st.hotkey !== undefined) {
    clipHotkey = st.hotkey || "";
    $("hotkey-hint").textContent = clipHotkey;
    $("hotkey-hint").classList.toggle("hidden", !clipHotkey);
    updateEmptyHotkey();
  }
  if (st.desktop !== undefined) {
    pcOn = !!st.desktop;
    $("aud-pc").classList.toggle("on", pcOn);
    $("aud-pc").setAttribute("aria-pressed", String(pcOn));
  }
  if (st.mic !== undefined) {
    micOn = !!st.mic;
    $("aud-mic").classList.toggle("on", micOn);
    $("aud-mic").setAttribute("aria-pressed", String(micOn));
  }
}

function showToast(text) {
  toastEl.textContent = text;
  toastEl.classList.remove("hidden");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toastEl.classList.add("hidden"), 3200);
}

function closePlayer() {
  try { video.pause(); } catch (e) {}
  video.removeAttribute("src");
  try { video.load(); } catch (e) {}
  hideDialog(overlay);
}

grid.addEventListener("click", (e) => {
  const card = e.target.closest(".card");
  if (!card) return;
  currentId = card.dataset.id;
  post({ cmd: "play", id: currentId });
});

$("search").addEventListener("input", render);
$("btn-clipnow").addEventListener("click", () => post({ cmd: "clip_now" }));
$("btn-settings").addEventListener("click", openSettings);
if ($("capture-settings")) $("capture-settings").addEventListener("click", openSettings);
if ($("nav-library")) $("nav-library").addEventListener("click", () => {
  selectedGame = "";
  updateGameSelection();
  showAppView("library");
  render();
});
if ($("nav-capture")) $("nav-capture").addEventListener("click", () => showAppView("capture"));
if ($("game-filters")) $("game-filters").addEventListener("click", (event) => {
  const button = event.target.closest("button[data-game]");
  if (!button) return;
  selectedGame = button.dataset.game;
  updateGameSelection();
  showAppView("library");
  render();
});
if ($("library-sort")) $("library-sort").addEventListener("change", render);
if ($("view-grid")) $("view-grid").addEventListener("click", () => setLibraryView("grid"));
if ($("view-list")) $("view-list").addEventListener("click", () => setLibraryView("list"));
function openSettings() {
  selectSettingsPage("recording");
  post({ cmd: "get_settings" });
  showDialog($("settings-overlay"), $("s-replay"));
}
function closeSettings() {
  hideDialog($("settings-overlay"));
}
$("set-close").addEventListener("click", closeSettings);
$("set-cancel").addEventListener("click", closeSettings);
$("set-save").addEventListener("click", () => {
  const val = (id) => $(id).value;
  const checked = (id) => $(id).checked;
  post({
    cmd: "save_settings",
    replay: parseInt(val("s-replay"), 10) || 60,
    fps: parseInt(val("s-fps"), 10) || 60,
    bitrate: parseInt(val("s-bitrate"), 10) || 15,
    quality: val("s-quality"),
    desktop: checked("s-desktop"),
    mic: checked("s-mic"),
    hotkey: val("s-hotkey").trim() === "(none)" ? "" : val("s-hotkey").trim() || "F8",
    startup: checked("s-startup"),
    notifications: checked("s-notify"),
    idlecap: checked("s-idlecap"),
    gamesmaster: checked("s-gamesmaster"),
    games: [...document.querySelectorAll("#s-gamelist input[data-game]")]
      .map((i) => `${i.dataset.game}=${i.checked ? 1 : 0}`)
      .join(",")
  });
  post({ cmd: "get_settings" });
  closeSettings();
  showToast("Settings saved");
});

document.addEventListener("keydown", (e) => {
  if (e.defaultPrevented || e.key !== "Escape") return;
  if (!srcMenu.classList.contains("hidden") || !micPop.classList.contains("hidden") || !$("pv-more-menu").classList.contains("hidden")) {
    e.preventDefault();
    closeMenus(true);
    return;
  }
  const active = currentDialog();
  if (!active) return;
  e.preventDefault();
  if (active.overlay.id === "settings-overlay") closeSettings();
  else closePlayer();
});
overlay.querySelector(".player-actions").addEventListener("click", (e) => {
  const btn = e.target.closest("button");
  if (!btn) return;
  const act = btn.dataset.act;
  if (!act) return;
  closeMenus();
  if (act === "close") return closePlayer();
  if (!currentId) return;
  post({ cmd: act, id: currentId });
});

$("pv-more").addEventListener("click", () => {
  const menu = $("pv-more-menu");
  const opening = menu.classList.contains("hidden");
  closeMenus();
  menu.classList.toggle("hidden", !opening);
  $("pv-more").setAttribute("aria-expanded", String(opening));
});
video.addEventListener("loadstart", () => playerMediaState("loading", "Loading clip…"));
video.addEventListener("canplay", () => playerMediaState("ready"));
video.addEventListener("playing", () => playerMediaState("ready"));
video.addEventListener("error", () => {
  if (!overlay.classList.contains("hidden") && video.error) {
    playerMediaState("error", "Unable to play this clip", "Try again, or open the original file from More → Open folder.", true);
  }
});
$("pv-retry").addEventListener("click", () => {
  if (!video.src) return;
  if (video.error) video.load();
  video.play().catch(handlePlayerPlayFailure);
});
for (const key of ["recording", "audio", "capture", "general"]) {
  const tab = $("set-nav-" + key);
  tab.addEventListener("click", () => selectSettingsPage(key));
  tab.addEventListener("keydown", (event) => {
    if (!["ArrowUp", "ArrowDown", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    event.stopPropagation();
    const keys = ["recording", "audio", "capture", "general"];
    const i = keys.indexOf(key);
    const next = event.key === "Home" ? 0 : event.key === "End" ? keys.length - 1 :
      event.key === "ArrowUp" ? (i + keys.length - 1) % keys.length : (i + 1) % keys.length;
    selectSettingsPage(keys[next], true);
  });
}
if (window.chrome && window.chrome.webview && window.chrome.webview.addEventListener) {
  window.chrome.webview.addEventListener("message", (e) => window.__onNative(e.data));
}

/* ---------------- Source dropdown + audio toggles + mic preview ---------------- */
const srcMenu = document.createElement("div");
srcMenu.className = "popmenu hidden";
srcMenu.id = "src-menu";
srcMenu.tabIndex = -1;
srcMenu.setAttribute("role", "group");
srcMenu.setAttribute("aria-label", "Capture source");
document.body.appendChild(srcMenu);
const micPop = document.createElement("div");
micPop.className = "popmenu hidden";
micPop.id = "mic-pop";
micPop.tabIndex = -1;
micPop.setAttribute("role", "group");
micPop.setAttribute("aria-label", "Microphones");
document.body.appendChild(micPop);
const menuFocusRequest = new Map();
$("src-btn").setAttribute("aria-controls", srcMenu.id);
$("mic-pick").setAttribute("aria-controls", micPop.id);
$("src-btn").setAttribute("aria-expanded", "false");
$("mic-pick").setAttribute("aria-expanded", "false");

let displays = [];
let activeDisp = 0;
let micList = [];
let activeMicId = "";
let srcMode = "display";
let srcWinExe = "";
let srcWinTitle = "";
let srcWindows = [];

function anchorMenu(menu, btn) {
  const r = btn.getBoundingClientRect();
  menu.style.visibility = "hidden";
  menu.classList.remove("hidden");
  menu.style.minWidth = "";
  menu.style.maxWidth = "";
  const css = getComputedStyle(menu);
  const availableWidth = Math.max(0, window.innerWidth - 16);
  menu.style.maxWidth = Math.min(parseFloat(css.maxWidth) || availableWidth, availableWidth) + "px";
  if ((parseFloat(css.minWidth) || 0) > availableWidth) menu.style.minWidth = availableWidth + "px";
  const below = Math.max(0, window.innerHeight - r.bottom - 14);
  const above = Math.max(0, r.top - 14);
  menu.style.maxHeight = Math.max(below, above) + "px";
  menu.style.overflowY = "auto";
  const mw = menu.offsetWidth;
  const mh = menu.offsetHeight;
  const left = Math.max(8, Math.min(r.right - mw, window.innerWidth - mw - 8));
  const desiredTop = mh > below && above > below ? r.top - mh - 6 : r.bottom + 6;
  menu.style.left = left + "px";
  menu.style.top = Math.max(8, Math.min(desiredTop, window.innerHeight - mh - 8)) + "px";
  menu.style.visibility = "visible";
  btn.setAttribute("aria-expanded", "true");
}
function closeMenus(restoreFocus = false) {
  const open = !micPop.classList.contains("hidden") ? micPop :
    (!srcMenu.classList.contains("hidden") ? srcMenu :
      (!$("pv-more-menu").classList.contains("hidden") ? $("pv-more-menu") : null));
  $("pv-more-menu").classList.add("hidden");
  $("pv-more").setAttribute("aria-expanded", "false");
  srcMenu.classList.add("hidden");
  if (!micPop.classList.contains("hidden")) {
    micPop.classList.add("hidden");
    post({ cmd: "mic_preview_off" });
  }
  $("src-btn").setAttribute("aria-expanded", "false");
  $("mic-pick").setAttribute("aria-expanded", "false");
  menuFocusRequest.clear();
  if (restoreFocus && open) focusVisible($(open === srcMenu ? "src-btn" : open === micPop ? "mic-pick" : "pv-more"));
}

function rememberMenuFocus(menu) {
  const active = document.activeElement;
  return { inside: menu.contains(active), data: active.dataset ? { ...active.dataset } : {} };
}

function focusMenuItem(menu, direction, remembered) {
  const items = [...menu.querySelectorAll("button.mi")];
  const same = remembered && items.find((item) =>
    Object.entries(remembered).some(([key, value]) => item.dataset[key] === value));
  const target = same || (direction === "last" ? items[items.length - 1] :
    (direction === "first" ? items[0] : items.find((item) => item.getAttribute("aria-pressed") === "true") || items[0]));
  if (target) target.focus({ preventScroll: true });
  else menu.focus({ preventScroll: true });
}

function finishMenuRender(menu, trigger, remembered) {
  if (menu.classList.contains("hidden")) return;
  anchorMenu(menu, trigger);
  if (remembered.inside || menuFocusRequest.has(menu)) {
    focusMenuItem(menu, menuFocusRequest.get(menu) || "selected", remembered.data);
    menuFocusRequest.delete(menu);
  }
}

function bindMenuKeyboard(menu, trigger) {
  trigger.addEventListener("keydown", (event) => {
    if (!["ArrowDown", "ArrowUp"].includes(event.key)) return;
    event.preventDefault();
    const direction = event.key === "ArrowUp" ? "last" : "first";
    if (menu.classList.contains("hidden")) {
      trigger.click();
      menuFocusRequest.set(menu, direction);
    }
    focusMenuItem(menu, direction);
  });
  menu.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      closeMenus(true);
    } else if (event.key === "Tab") {
      closeMenus(true);
    } else if (["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) {
      event.preventDefault();
      event.stopPropagation();
      const items = [...menu.querySelectorAll("button.mi")];
      if (!items.length) return;
      const index = items.indexOf(document.activeElement);
      const next = event.key === "Home" ? 0 : event.key === "End" ? items.length - 1 :
        event.key === "ArrowDown" ? (index + 1) % items.length : (index <= 0 ? items.length - 1 : index - 1);
      items[next].focus();
    }
  });
}
bindMenuKeyboard(srcMenu, $("src-btn"));
bindMenuKeyboard(micPop, $("mic-pick"));
bindMenuKeyboard($("pv-more-menu"), $("pv-more"));
window.addEventListener("resize", () => {
  if (!srcMenu.classList.contains("hidden")) anchorMenu(srcMenu, $("src-btn"));
  if (!micPop.classList.contains("hidden")) anchorMenu(micPop, $("mic-pick"));
});
document.addEventListener("click", (e) => {
  const path = e.composedPath();
  if (!e.target.closest("#src-btn") && !path.includes(srcMenu) &&
      !e.target.closest(".mic-wrap") && !path.includes(micPop) &&
      !e.target.closest(".player-more-wrap")) closeMenus();
});

$("src-btn").addEventListener("click", () => {
  if (!srcMenu.classList.contains("hidden")) return closeMenus(true);
  closeMenus();
  post({ cmd: "list_sources" });
  srcMenu.innerHTML = '<div class="empty">Loading…</div>';
  anchorMenu(srcMenu, $("src-btn"));
  srcMenu.focus({ preventScroll: true });
});
$("aud-pc").classList.toggle("on", true);
$("aud-pc").setAttribute("aria-pressed", String(pcOn));
$("aud-mic").setAttribute("aria-pressed", String(micOn));
$("aud-pc").addEventListener("click", () => {
  pcOn = !pcOn;
  $("aud-pc").classList.toggle("on", pcOn);
  $("aud-pc").setAttribute("aria-pressed", String(pcOn));
  post({ cmd: "set_audio", desktop: pcOn, mic: micOn });
});
$("aud-mic").addEventListener("click", () => {
  micOn = !micOn;
  $("aud-mic").classList.toggle("on", micOn);
  $("aud-mic").setAttribute("aria-pressed", String(micOn));
  post({ cmd: "set_audio", desktop: pcOn, mic: micOn });
});
$("mic-pick").addEventListener("click", () => {
  if (!micPop.classList.contains("hidden")) return closeMenus(true);
  closeMenus();
  post({ cmd: "list_mics" });
  post({ cmd: "mic_preview_on" });
  micPop.innerHTML = '<div class="empty">Loading devices…</div>';
  anchorMenu(micPop, $("mic-pick"));
  micPop.focus({ preventScroll: true });
});

function shortTitle(t) {
  t = String(t || "");
  return t.length > 26 ? t.slice(0, 25) + "…" : t;
}
function renderSrcMenu() {
  const remembered = rememberMenuFocus(srcMenu);
  const dispRows = displays.map((d) => `
        <button type="button" class="mi sub ${srcMode === "display" && d.i === activeDisp ? "active" : ""}" data-disp="${d.i}" aria-pressed="${srcMode === "display" && d.i === activeDisp}">
          <span>${esc(d.label)}</span>${srcMode === "display" && d.i === activeDisp ? '<span aria-hidden="true">✓</span>' : ""}
        </button>`).join("");
  const winRows = srcWindows.length ? srcWindows.map((w, i) => {
    const sel = srcMode === "window" && w.exe === srcWinExe &&
      (!srcWinTitle || w.title === srcWinTitle);
    return `
        <button type="button" class="mi ${sel ? "active" : ""}" data-win="${i}" aria-pressed="${sel}" title="${esc(w.title || w.exe)}">
          <span class="col"><span>${esc(shortTitle(w.title))}</span>` +
          `<span class="sub2">${esc(w.exe)}</span></span>${sel ? '<span aria-hidden="true">✓</span>' : ""}
        </button>`;
  }).join("") : '<div class="empty">No windows available.</div>';
  srcMenu.innerHTML =
    `<button type="button" class="mi ${srcMode === "display" ? "active" : ""}" data-mode="display" aria-pressed="${srcMode === "display"}">
       <span>Entire desktop</span>${srcMode === "display" ? '<span aria-hidden="true">✓</span>' : ""}
     </button>` + dispRows +
    `<div class="mhead">Window</div>` + winRows;
  srcMenu.querySelectorAll("[data-mode]").forEach((el) =>
    el.addEventListener("click", () => {
      post({ cmd: "set_source", mode: "display", display: activeDisp });
      closeMenus(true);
    }));
  srcMenu.querySelectorAll("[data-disp]").forEach((el) =>
    el.addEventListener("click", () => {
      activeDisp = parseInt(el.dataset.disp, 10) || 0;
      post({ cmd: "set_source", mode: "display", display: activeDisp });
      closeMenus(true);
    }));
  srcMenu.querySelectorAll("[data-win]").forEach((el) =>
    el.addEventListener("click", () => {
      const w = srcWindows[parseInt(el.dataset.win, 10)];
      if (!w) return;
      post({ cmd: "set_source", mode: "window", exe: w.exe, title: w.title });
      closeMenus(true);
    }));
  finishMenuRender(srcMenu, $("src-btn"), remembered);
}
function updateSrcLabel() {
  updateCaptureSummary();
  if (srcMode === "window" && (srcWinTitle || srcWinExe)) {
    $("src-label").textContent = shortTitle(srcWinTitle || srcWinExe);
    return;
  }
  const d = displays.find((x) => x.i === activeDisp);
  $("src-label").textContent = d ? d.label.split("·")[0].trim() : "Display";
}

function renderMics() {
  const remembered = rememberMenuFocus(micPop);
  micPop.innerHTML = micList.length
    ? `<div class="empty" style="padding-bottom:4px">Speak to see levels · click to pick</div>` +
      micList.map((m) => `
        <button type="button" class="mi ${m.id === activeMicId ? "active" : ""}" data-id="${esc(m.id)}" aria-pressed="${m.id === activeMicId}" title="${esc(m.name || m.id)}">
          <span class="microw ${m.id === activeMicId ? "sel" : ""}" data-id="${esc(m.id)}">
            <span class="name">${esc(m.name || m.id)}</span>
            <span class="meter" aria-hidden="true"><i data-id="${esc(m.id)}"></i></span>
          </span>
        </button>`).join("")
    : '<div class="empty">No microphones found.</div>';
  [...micPop.querySelectorAll("button[data-id]")].forEach((el) =>
    el.addEventListener("click", () => {
      activeMicId = el.dataset.id;
      post({ cmd: "set_mic", id: activeMicId });
      renderMics();
    }));
  finishMenuRender(micPop, $("mic-pick"), remembered);
}
const hkField = $("s-hotkey");
let hkListening = false;
hkField.addEventListener("focus", () => {
  hkListening = true;
  hkField.placeholder = "Press keys… (Esc=cancel · Backspace=unbind)";
  hkField.classList.add("listening");
});
function hkStop() {
  hkListening = false;
  hkField.classList.remove("listening");
  hkField.placeholder = "Click to set";
  hkField.blur();
}
document.addEventListener("keydown", (e) => {
  if (!hkListening) return;
  e.preventDefault();
  e.stopPropagation();
  if (e.key === "Escape") return hkStop();
  if (e.key === "Backspace") { hkField.value = "(none)"; return hkStop(); }
  const parts = [];
  if (e.ctrlKey) parts.push("Ctrl");
  if (e.shiftKey) parts.push("Shift");
  if (e.altKey) parts.push("Alt");
  let k = e.key;
  if (["Control", "Shift", "Alt", "Meta"].includes(k)) return;  // wait for the real key
  if (k === " ") k = "Space";
  else if (k.length === 1) k = k.toUpperCase();
  parts.push(k);
  hkField.value = parts.join("+");
  hkStop();
}, true);

showAppView("library");
setLibraryView("grid");
updateCaptureSummary();
post({ cmd: "ready" });
post({ cmd: "get_settings" });
post({ cmd: "list_sources" });
