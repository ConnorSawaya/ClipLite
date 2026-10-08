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

function selectInspectorPanel(key, moveFocus = false) {
  const keys = ["video", "text", "export"];
  if (!keys.includes(key)) return;
  const panels = [...document.querySelectorAll("[data-inspector-panel]")];
  const focusWillHide = panels.some((panel) =>
    panel.dataset.inspectorPanel !== key && panel.contains(document.activeElement));
  for (const name of keys) {
    const tab = $("inspector-" + name);
    if (!tab) continue;
    tab.setAttribute("aria-selected", String(name === key));
    tab.tabIndex = name === key ? 0 : -1;
    const panel = panels.find((item) => item.dataset.inspectorPanel === name);
    if (panel) {
      if (!panel.id) panel.id = "inspector-" + name + "-panel";
      tab.setAttribute("aria-controls", panel.id);
      panel.setAttribute("role", "tabpanel");
      panel.setAttribute("aria-labelledby", tab.id);
      panel.classList.toggle("hidden", name !== key);
      panel.hidden = name !== key;
      panel.inert = name !== key;
    }
  }
  if (moveFocus || focusWillHide) focusVisible($("inspector-" + key));
}

function setInspectorOpen(open, moveFocus = false) {
  const pane = $("e-inspector");
  const toggle = $("e-inspector-toggle");
  const focusedInside = pane.contains(document.activeElement);
  $("edit-overlay").querySelector(".editor-shell").classList.toggle("inspector-open", open);
  toggle.setAttribute("aria-expanded", String(open));
  pane.inert = window.innerWidth <= 800 && !open;
  if (open && moveFocus) focusVisible(pane.querySelector('[role="tab"][aria-selected="true"]'));
  else if (!open && focusedInside) focusVisible(toggle);
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
      showDialog(overlay, overlay.querySelector('[data-act="edit"]'));
      video.play().catch(handlePlayerPlayFailure);
      break;
    }
    case "progress": {
      const w = $("e-progress-wrap");
      if (w.classList.contains("hidden")) {
        w.classList.remove("hidden");
        showDialog(editOv, $("e-progress-cancel"));
      }
      $("e-progress-bar").style.transform = "scaleX(" + Math.min(100, Math.max(0, m.pct || 0)) / 100 + ")";
      $("e-progress-text").textContent = "Rendering... " + (m.pct || 0) + "%";
      eSetSaveState("Rendering clip", "pending");
      if ($("e-render-summary")) $("e-render-summary").textContent = "Exporting a new clip. The original stays intact.";
      break;
    }
    case "edit_done":
      $("e-progress-wrap").classList.add("hidden");
      eSetSaveState(m.ok ? "Render complete" : "Render failed", m.ok ? "complete" : "pending");
      if ($("e-render-summary")) $("e-render-summary").textContent = m.ok
        ? "New clip saved to your library."
        : "The clip could not be rendered. Your edits are still here.";
      showToast(m.ok ? "Saved " + (m.name || "edited clip") : "Render failed");
      break;
    case "editor_data": {
      if (m.id !== editId) break;
      eDur = (m.dur_ms || 0) / 1000;
      if (!(eOut > 0)) eOut = eDur;
      if (eFresh && eDur > 0) {
        // True duration arrived before any user edit: rebase the project.
        // (A saved project, if present, is restored below after tracks load.
        // No history push here: the push happens once after restore-or-rebase
        // so the restore gate below can still fire.)
        eClips = [{ id: "c0", src: 0, s: 0, e: Math.round(eDur * 1000) / 1000 }];
        eSources = [{ id: editId, name: eClipLabel(editId) || "This clip", dur: eDur }];
        eVideoSrcId = editId;
        eRefreshImportList();
        eSel = 0;
        eHist = [];
        eHistIx = -1;
        selectSeg(0, false);
      }
      renderEditRuler();
      const film = m.film || [];
      $("e-film").innerHTML = film.length
        ? film.map((u) => `<img src="${u}" alt="" draggable="false">`).join("")
        : '<div class="filmstrip-state">Preview frames unavailable - timeline editing still works.</div>';
      eTracks = (m.tracks || []).map((t) => ({
        pid: t.pid, exe: t.exe || "", removable: !!t.removable,
        keep: true, peaks: t.peaks || []
      }));
      eAudioReady = true;
      renderEditTracks();
      syncEditTimeline();
      if (eFresh && m.saved && eRestoreSaved(m.saved)) {
        showToast("Recovered autosaved edits");
        ePushHist();
      } else if (eFresh) {
        // Fresh open with no save: record the rebased baseline once.
        ePushHist();
      }
      eFresh = false;
      eUpdateEditorChrome();
      break;
    }
    case "app_audio":
      renderAppAudio(m.id, m.apps || []);
      break;
    case "app_audio_done":
      showToast(m.ok ? "Saved " + (m.name || "clean mix") : "Mixdown failed");
      break;
    case "preview_url":
      if (ePendingSrcId === null || m.id !== ePendingSrcId) break;
      ePendingSrcId = null;
      if (m.url) {
        $("e-video").src = m.url;
        if (ePendingSeek !== null) {
          const t = ePendingSeek;
          ePendingSeek = null;
          const v = $("e-video");
          const apply = () => { try { v.currentTime = t; } catch (e) {} };
          if (v.readyState >= 1) apply();
          else v.onloadedmetadata = apply;
        }
      }
      break;
    case "trim_done":
      showToast(m.ok ? "Saved " + (m.name || "trimmed clip") : "Trim failed");
      break;
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
for (const key of ["video", "text", "export"]) {
  const tab = $("inspector-" + key);
  if (!tab) continue;
  tab.addEventListener("click", () => selectInspectorPanel(key));
  tab.addEventListener("keydown", (event) => {
    if (!["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    event.stopPropagation();
    const keys = ["video", "text", "export"];
    const index = keys.indexOf(key);
    const next = event.key === "Home" ? 0 : event.key === "End" ? keys.length - 1 :
      ["ArrowLeft", "ArrowUp"].includes(event.key) ? (index + keys.length - 1) % keys.length : (index + 1) % keys.length;
    selectInspectorPanel(keys[next], true);
  });
}

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
  else if (active.overlay.id === "appaudio-overlay") closeAppAudio();
  else if (active.overlay.id === "trim-overlay") closeTrim();
  else if (active.overlay.id === "edit-overlay") closeEditor();
  else closePlayer();
});
// Double-clicking the playing video jumps straight into the full editor.
video.addEventListener("dblclick", () => {
  if (!currentId) return;
  editId = currentId;
  openEditor();
});

overlay.querySelector(".player-actions").addEventListener("click", (e) => {
  const btn = e.target.closest("button");
  if (!btn) return;
  const act = btn.dataset.act;
  if (!act) return;
  closeMenus();
  if (act === "close") return closePlayer();
  if (!currentId) return;
  if (act === "edit") { editId = currentId; openEditor(); return; }
  if (act === "trim") { openTrim(currentId); return; }
  if (act === "app_audio") { openAppAudio(currentId); return; }
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
$("e-inspector-toggle").addEventListener("click", () => {
  setInspectorOpen($("e-inspector-toggle").getAttribute("aria-expanded") !== "true", true);
});
window.matchMedia("(max-width: 800px)").addEventListener("change", (event) => setInspectorOpen(!event.matches));

if (window.chrome && window.chrome.webview && window.chrome.webview.addEventListener) {
  window.chrome.webview.addEventListener("message", (e) => window.__onNative(e.data));
}

/* ---------------- Editor ---------------- */
const editOv = $("edit-overlay");
let editId = null;
let editDurMs = 0;
let editRegions = [];
let editSel = -1;

let eDur = 0;
let eIn = 0;
let eOut = 0;
let eTracks = [];
let eShowMixedTracks = false;
const editorRemoveIcon = '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><path d="m6 6 12 12M6 18 18 6"/></svg>';
let eDrag = null;
// Multi-segment project (single source, cuts only). eIn/eOut always mirror
// the SELECTED segment; render posts every segment in order.
let eClips = [];
let eSel = 0;
let eSeq = 0;
let eHist = [];
let eHistIx = -1;
let eFresh = true;
// Multi-source timeline: eSources[0] is always the clip being edited;
// imports append {id, name, dur}. Clips carry src = source index.
let eSources = [];
let eVideoSrcId = null;
let eAudioReady = false;

function eSetSaveState(text, state) {
  const el = $("e-save-state");
  if (!el) return;
  el.textContent = text;
  el.dataset.state = state || "ready";
}
function eUpdateEditorChrome() {
  const c = eClips[eSel];
  const count = eClips.length;
  const total = eRipple().total / 1000;
  const hasSelection = !!c;
  const summary = hasSelection
    ? `Segment ${eSel + 1} of ${count} - ${eFmt(eSegOutMs(c) / 1000)} output`
    : "No segment selected";
  if ($("e-segment-summary")) $("e-segment-summary").textContent = summary;
  if ($("e-inspector-detail")) {
    $("e-inspector-detail").textContent = hasSelection
      ? `${eFmt(c.s)} to ${eFmt(c.e)} source - ${eFmt(total)} total`
      : "Choose a segment on the timeline to edit it.";
  }
  if ($("e-preview-mode")) {
    $("e-preview-mode").textContent = hasSelection
      ? `Selected segment - ${(c.spd || 1)}x preview`
      : "Selected segment preview";
  }
  if ($("e-undo")) $("e-undo").disabled = eHistIx <= 0;
  if ($("e-redo")) $("e-redo").disabled = eHistIx < 0 || eHistIx >= eHist.length - 1;
  if ($("e-split")) $("e-split").disabled = !hasSelection;
  if ($("e-delete")) $("e-delete").disabled = count <= 1;
  if ($("e-move-l")) $("e-move-l").disabled = !hasSelection || eSel <= 0;
  if ($("e-move-r")) $("e-move-r").disabled = !hasSelection || eSel >= count - 1;
  if ($("e-speed")) $("e-speed").disabled = !hasSelection;
  if ($("e-play")) $("e-play").disabled = !hasSelection;
  if ($("edit-render")) $("edit-render").disabled = !hasSelection;
}

function eSegOutMs(c) {
  // Integer-ms output duration, mirroring native llround((e-s)/speed):
  // Math.round and llround agree exactly on non-negative values, so the
  // previewed total always equals the rendered file length.
  return Math.max(0, Math.round((c.e - c.s) * 1000 / (c.spd || 1)));
}
function eRipple() {
  const starts = [];
  let t = 0;
  for (const c of eClips) {
    starts.push(t);
    t += eSegOutMs(c);
  }
  return { starts, total: t };
}
function eSrcDur(si) {
  if (si === 0) return eDur;
  const s = eSources[si];
  return s && s.dur > 0 ? s.dur : 0;
}
function tFrac(t, total) {
  return total > 0 ? Math.min(1, Math.max(0, t / total)) : 0;
}


function eClipLabel(id) {
  const c = clips.find((x) => x.id === id);
  if (!c) return null;
  return (c.game || c.file || c.id) + (c.date ? " · " + c.date : "");
}
function eRefreshImportList() {}  // import UI removed; saved imports still render
function eSnapshot() {
  return JSON.stringify({
    clips: eClips, sel: eSel,
    sources: eSources.map((s) => s.id),
    canvas: [$("e-crop-preset") ? $("e-crop-preset").value : "full",
             $("e-canvas-fit") ? $("e-canvas-fit").value : "0"],
    texts: editTexts.map((t) => ({
      content: t.content, size: t.size, color: t.color, font: t.font,
      px: t.px, py: t.py, align: t.align, start: t.start, end: t.end
    })),
    tracks: eTracks.map((t) => ({ keep: t.keep, gain: t.gain, solo: !!t.solo })),
    exp: [$("e-res") ? $("e-res").value : "0",
          $("e-fps") ? $("e-fps").value : "0",
          $("e-quality") ? $("e-quality").value : "1"],
    regions: editRegions,
    crop: $("e-crop-preset") ? $("e-crop-preset").value : "full",
    custom: ["e-cx", "e-cy", "e-cw", "e-chh"].map((id) => $(id) ? $(id).value : "")
  });
}
function restoreCanvas(s) {
  if ($("e-crop-preset") && s.canvas && s.canvas[0]) $("e-crop-preset").value = s.canvas[0];
  else if ($("e-crop-preset")) $("e-crop-preset").value = s.crop || "full";
  if ($("e-canvas-fit")) $("e-canvas-fit").value = (s.canvas && s.canvas[1] === "1") ? "1" : "0";
  syncCanvasRow();
}
function eBumpSeq() {
  // Keep generated ids ("cN") unique after restores that carry their own.
  for (const c of eClips) {
    const m = c && c.id && /^c(\d+)$/.exec(c.id);
    if (m && +m[1] >= eSeq) eSeq = +m[1];
  }
}
function eRestore(snap) {
  const s = JSON.parse(snap);
  // Undo across an import must restore the source table too, or segments
  // point at the wrong files. Unknown ids drop with their segments below.
  eRestoreSources(s);
  eClips = Array.isArray(s.clips) ? s.clips : [];
  for (const c of eClips) {
    if (!c || typeof c !== "object") continue;
    if (!(c.src >= 0) || c.src >= eSources.length) c.src = 0;
    if (!(c.spd >= 0.25 && c.spd <= 4)) c.spd = 1;
  }
  eClips = eClips.filter((c) => c && typeof c === "object");
  eBumpSeq();
  eSel = Math.min(Math.max(0, s.sel | 0), Math.max(0, eClips.length - 1));
  (s.tracks || []).forEach((st, i) => {
    if (eTracks[i]) {
      eTracks[i].keep = !!st.keep;
      eTracks[i].gain = st.gain === undefined ? 1 : st.gain;
      eTracks[i].solo = !!st.solo;
    }
  });
  editRegions = s.regions || [];
  editTexts = [];
  textSel = -1;
  if (Array.isArray(s.texts)) {
    for (const t of s.texts) {
      if (!t || typeof t !== "object") continue;
      const content = String(t.content || "").slice(0, 200);
      if (!content) continue;
      editTexts.push({
        content,
        size: ["S", "M", "L"].includes(t.size) ? t.size : "M",
        color: /^#[0-9a-fA-F]{6}$/.test(t.color || "") ? t.color : "#ffffff",
        font: String(t.font || "Segoe UI"),
        px: [0, 1, 2].includes(t.px | 0) ? (t.px | 0) : 1,
        py: [0, 1, 2].includes(t.py | 0) ? (t.py | 0) : 2,
        align: ["left", "center", "right"].includes(t.align) ? t.align : "center",
        start: Math.min(Math.max(0, +t.start || 0), Math.max(eDur, 0.1)),
        end: Math.min(Math.max(0, +t.end || 0), Math.max(eDur, 0.1))
      });
    }
    if (editTexts.length) textSel = 0;
  }
  renderTextList();
  syncTextEditor();
  editSel = -1;
  ["e-cx", "e-cy", "e-cw", "e-chh"].forEach((id, i) => { if ($(id)) $(id).value = (s.custom || [])[i] || ""; });
  restoreCanvas(s);
  renderEditRegions();
  renderEditTracks();
  selectSeg(eSel, false);
}
let eSaveTimer = 0;
function eScheduleAutosave() {
  clearTimeout(eSaveTimer);
  eSetSaveState("Changes pending", "pending");
  eSaveTimer = setTimeout(() => {
    eSaveTimer = 0;
    if (editId) {
      post({ cmd: "project_save", id: editId, project: eSnapshot() });
      eSetSaveState("Changes sent to project", "sent");
    }
  }, 800);
}
function eFlushAutosave() {
  if (!eSaveTimer) return;
  clearTimeout(eSaveTimer);
  eSaveTimer = 0;
  if (editId) {
    post({ cmd: "project_save", id: editId, project: eSnapshot() });
    eSetSaveState("Changes sent to project", "sent");
  }
}
function ePushHist(schedule = true) {
  eFresh = false;  // any committed state outruns a late editor_data rebase
  eHist = eHist.slice(0, eHistIx + 1);
  eHist.push(eSnapshot());
  if (eHist.length > 50) eHist.shift();
  eHistIx = eHist.length - 1;
  if (schedule) eScheduleAutosave();
  eUpdateEditorChrome();
}
function eRestoreSources(sv) {
  // Rebuilds eSources from saved clip ids; unknown ids are dropped with
  // their segments. Returns false when nothing usable remains.
  const ids = [editId];
  for (const cid of (sv.sources || [])) {
    if (cid && cid !== editId && !ids.includes(cid)) ids.push(cid);
  }
  eSources = [{ id: editId, name: eClipLabel(editId) || "This clip", dur: eDur }];
  for (let k = 1; k < ids.length; ++k) {
    const c = clips.find((x) => x.id === ids[k]);
    if (c && c.dur_ms > 0) {
      eSources.push({ id: ids[k], name: eClipLabel(ids[k]) || ids[k], dur: c.dur_ms / 1000 });
    }
  }
  eRefreshImportList();
  return true;
}
function eRestoreSaved(sv) {
  if (!sv || !Array.isArray(sv.clips) || !sv.clips.length || !(eDur > 0)) return false;
  eRestoreSources(sv);
  const clips = [];
  for (const c of sv.clips) {
    if (!c || typeof c !== "object") continue;
    let si = c.src | 0;
    if (si < 0 || si >= eSources.length) continue;
    const srcDur = eSrcDur(si);
    if (!(srcDur > 0)) continue;
    let s = +c.s, e = +c.e;
    if (!(e > s)) continue;
    s = Math.min(Math.max(0, s), srcDur);
    e = Math.min(Math.max(0, e), srcDur);
    if (!(e - s >= 0.1)) continue;
    const spd = +c.spd;
    clips.push({
      id: String(c.id || ("c" + (++eSeq))),
      src: si,
      s: Math.round(s * 1000) / 1000,
      e: Math.round(e * 1000) / 1000,
      spd: (spd >= 0.25 && spd <= 4) ? spd : 1
    });
  }
  if (!clips.length) return false;
  eClips = clips;
  eBumpSeq();
  eSel = Math.min(Math.max(0, sv.sel | 0), clips.length - 1);
  eVideoSrcId = null;  // force preview re-resolve on next select
  (sv.tracks || []).forEach((st, i) => {
    if (eTracks[i]) {
      eTracks[i].keep = !!st.keep;
      eTracks[i].gain = st.gain === undefined ? 1 : st.gain;
      eTracks[i].solo = !!st.solo;
    }
  });
  if (Array.isArray(sv.exp)) {
    if ($("e-res")) $("e-res").value = ["0", "720", "1080"].includes(String(sv.exp[0])) ? String(sv.exp[0]) : "0";
    if ($("e-fps")) $("e-fps").value = ["0", "30", "60"].includes(String(sv.exp[1])) ? String(sv.exp[1]) : "0";
    if ($("e-quality")) $("e-quality").value = ["0", "1", "2"].includes(String(sv.exp[2])) ? String(sv.exp[2]) : "1";
  }
  ["e-cx", "e-cy", "e-cw", "e-chh"].forEach((id, i) => {
    if ($(id)) $(id).value = ((sv.custom || [])[i] || "");
  });
  restoreCanvas(sv);
  editRegions = Array.isArray(sv.regions) ? sv.regions : [];
  editSel = -1;
  eFresh = false;
  renderEditRegions();
  renderEditTracks();
  selectSeg(eSel, false);
  return true;
}
function eUndo() {
  if (eHistIx <= 0) { showToast("Nothing to undo"); return; }
  eHistIx--;
  eRestore(eHist[eHistIx]);
  eScheduleAutosave();
  eUpdateEditorChrome();
}
function eRedo() {
  if (eHistIx >= eHist.length - 1) { showToast("Nothing to redo"); return; }
  eHistIx++;
  eRestore(eHist[eHistIx]);
  eScheduleAutosave();
  eUpdateEditorChrome();
}
function renderSegBlocks() {
  const layer = $("e-segs");
  layer.innerHTML = "";
  const { starts, total } = eRipple();
  eClips.forEach((c, i) => {
    const d = document.createElement("button");
    d.type = "button";
    d.className = "seg" + (i === eSel ? " sel" : "") + (c.src > 0 ? " imp" : "");
    d.style.left = (tFrac(starts[i], total) * 100) + "%";
    d.style.width = (Math.max(0.5, (tFrac(starts[i] + eSegOutMs(c), total) - tFrac(starts[i], total)) * 100)) + "%";
    const sp = c.spd || 1;
    const srcTag = c.src > 0 ? " ◦" + (c.src + 1) : "";
    d.textContent = (eClips.length > 1 || c.src > 0) ? (i + 1) + (sp !== 1 ? " " + sp + "×" : "") + srcTag : ((sp !== 1 ? sp + "×" : "") + srcTag);
    d.setAttribute("aria-label", "Segment " + (i + 1) + ", " + eFmt(c.s) + " to " + eFmt(c.e) + (sp !== 1 ? ", " + sp + " times speed" : ""));
    d.setAttribute("aria-pressed", i === eSel ? "true" : "false");
    d.title = "Segment " + (i + 1) + " (" + eFmt(c.s) + "–" + eFmt(c.e) +
      (sp !== 1 ? ", " + sp + "× → " + eFmt(eSegOutMs(c) / 1000) : "") +
      (c.src > 0 && eSources[c.src] ? ", " + eSources[c.src].name : "") + "). Click to select.";
    d.addEventListener("pointerdown", (ev) => { ev.stopPropagation(); });
    d.addEventListener("click", (ev) => { ev.stopPropagation(); selectSegSrc(i); });
    layer.appendChild(d);
  });
  $("e-tlen").textContent = "→ " + eFmt(Math.max(0, eOut - eIn)) + " selected · " +
    eFmt(total / 1000) + " total";
  redrawTrackWaves();
  eUpdateEditorChrome();
}
function eSegSourceId(i) {
  const c = eClips[i];
  if (!c) return null;
  const s = eSources[c.src || 0];
  return s ? s.id : null;
}
let ePendingSeek = null;
let ePendingSrcId = null;
function ensureVideoSrc(i, seekTo) {
  const id = eSegSourceId(i);
  if (!id) return;
  if (seekTo !== undefined) ePendingSeek = seekTo;
  if (eVideoSrcId !== id) {
    eVideoSrcId = id;
    ePendingSrcId = id;
    if (id === editId && video.src) {
      $("e-video").src = video.src;
      ePendingSrcId = null;
      if (ePendingSeek !== null) {
        const t = ePendingSeek;
        ePendingSeek = null;
        try { $("e-video").currentTime = t; } catch (e) {}
      }
    } else {
      post({ cmd: "preview_source", id });
    }
  } else if (seekTo !== undefined) {
    ePendingSeek = null;
    try { $("e-video").currentTime = seekTo; } catch (e) {}
  }
}
function selectSeg(i, seek) {
  if (!eClips.length) return;
  eSel = Math.min(Math.max(0, i), eClips.length - 1);
  const c = eClips[eSel];
  if (!(c.spd >= 0.25 && c.spd <= 4)) c.spd = 1;
  eIn = c.s;
  eOut = c.e;
  if ($("e-speed")) $("e-speed").value = String(c.spd);
  syncEditTimeline();
  renderSegBlocks();
  eSyncPreviewMode();
  renderTextPreview();
  if (seek) ensureVideoSrc(eSel, c.s);
}
function selectSegSrc(i) {
  // Clicking a block selects it AND seeks its own source media.
  selectSeg(i, false);
  const c = eClips[eSel];
  ensureVideoSrc(eSel, c.s);
  renderSegBlocks();
}
function segIndexAt(t) {
  const { starts, total } = eRipple();
  if (!(total > 0)) return -1;
  for (let i = eClips.length - 1; i >= 0; --i) {
    if (t >= starts[i]) return i;
  }
  return 0;
}
function eSplitAtPlayhead() {
  if (!eClips.length || !eClips[eSel]) return;
  const c = eClips[eSel];
  if (eVideoSrcId !== eSegSourceId(eSel)) {
    showToast("Play the selected segment first, then split");
    return;
  }
  const t = $("e-video").currentTime || 0;
  if (!(t > c.s + 0.05 && t < c.e - 0.05) || t - c.s < 0.1 || c.e - t < 0.1) {
    showToast("Put the playhead inside the selected segment to split (min 0.1s each side)");
    return;
  }
  const right = {
    id: "c" + (++eSeq), src: c.src || 0,
    s: Math.round(t * 1000) / 1000, e: c.e, spd: c.spd || 1
  };
  c.e = Math.round(t * 1000) / 1000;
  eClips.splice(eSel + 1, 0, right);
  selectSeg(eSel + 1, false);
  ePushHist();
  showToast("Split into " + eClips.length + " segments");
}
function eDeleteSel() {
  if (eClips.length <= 1) { showToast("A project needs at least one segment"); return; }
  eClips.splice(eSel, 1);
  selectSeg(Math.min(eSel, eClips.length - 1), false);
  ePushHist();
}
function eMoveSel(dir) {
  const j = eSel + dir;
  if (j < 0 || j >= eClips.length) return;
  const [c] = eClips.splice(eSel, 1);
  eClips.splice(j, 0, c);
  selectSeg(j, false);
  ePushHist();
}

function eFrac(t) {
  return eDur > 0 ? Math.min(1, Math.max(0, t / eDur)) : 0;
}
function eFmt(t) {
  return (Math.round(t * 10) / 10).toFixed(1) + "s";
}
function syncEditTimeline() {
  // Handles live in TIMELINE scale (selected segment's span); inputs show
  // its source times. Clamps follow the selected segment's own source.
  const c = eClips[eSel];
  const srcLo = 0, srcHi = c ? Math.max(0.1, eSrcDur(c.src || 0)) : Math.max(eDur, 0.1);
  if (!(eDur > 0) && !c) { eIn = 0; eOut = 0; }
  eIn = Math.min(Math.max(srcLo, eIn), Math.max(srcLo, eOut - 0.1));
  eOut = Math.min(Math.max(eIn + 0.1, eOut), Math.max(srcHi, 0.1));
  if (!(eOut > eIn)) { eIn = srcLo; eOut = Math.max(srcHi, srcLo + 0.1); }
  $("e-start").value = (Math.round(eIn * 10) / 10).toFixed(1);
  $("e-end").value = (Math.round(eOut * 10) / 10).toFixed(1);
  $("e-t0").textContent = eFmt(eIn);
  $("e-t1").textContent = eFmt(eOut);
  const { starts, total } = eRipple();
  const sp = (c && c.spd) || 1;
  // Ripple is integer-ms; source inputs are seconds: convert at the boundary.
  const t0 = c ? starts[eSel] + Math.round((eIn - c.s) * 1000 / sp) : 0;
  const t1 = c ? starts[eSel] + Math.round((eOut - c.s) * 1000 / sp) : 0;
  $("e-region").style.left = (tFrac(t0, total) * 100) + "%";
  $("e-region").style.width = (Math.max(0, (tFrac(t1, total) - tFrac(t0, total))) * 100) + "%";
  $("e-in").style.left = (tFrac(t0, total) * 100) + "%";
  $("e-out").style.left = (tFrac(t1, total) * 100) + "%";
  renderSegBlocks();
}
function eSeekEvt(e) {
  // Pointer position in TIMELINE integer-ms (ripple scale).
  const r = $("e-track").getBoundingClientRect();
  if (!(r.width > 0)) return 0;
  const { total } = eRipple();
  const span = total > 0 ? total : (eDur > 0 ? Math.round(eDur * 1000) : 100);
  return Math.round(span * Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)));
}
function eTimelineToSource(t) {
  // Timeline integer-ms -> {seg, source seconds} (last segment wins ties).
  const { starts } = eRipple();
  let i = segIndexAt(t);
  if (i < 0) i = 0;
  const c = eClips[i];
  if (!c) return null;
  const sp = c.spd || 1;
  return { i, v: c.s + (t - starts[i]) / 1000 * sp };
}
function eRulerSpan() {
  // Ruler always reads in timeline integer-ms so it agrees with blocks,
  // handles and playhead (single 1x projects are identical on either scale).
  if (eClips.length) return eRipple().total;
  return Math.round(eDur * 1000);
}
function renderEditRuler() {
  const ruler = $("e-ruler");
  ruler.innerHTML = "";
  const span = eRulerSpan();
  if (!(span > 0)) return;
  const steps = [1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000];
  let step = steps[steps.length - 1];
  for (const s of steps) {
    if (span / s <= 12) { step = s; break; }
  }
  const minor = step / 5;
  const frac = (t) => tFrac(t, span);
  const frag = document.createDocumentFragment();
  for (let t = 0; t <= span + 0.5; t += minor) {
    const major = Math.abs(t / step - Math.round(t / step)) < 1e-6;
    const d = document.createElement("div");
    d.className = "tick" + (major ? " major" : "");
    d.style.left = (frac(t) * 100) + "%";
    if (major) {
      const lab = document.createElement("span");
      lab.textContent = (Math.round(t / 100) / 10) + "s";
      d.appendChild(lab);
    }
    frag.appendChild(d);
  }
  ruler.appendChild(frag);
}

// Stable per-app hue so a track keeps its color between renders.
function eTrackColor(pid) {
  const hue = (pid * 47) % 360;
  return `hsl(${hue}, 70%, 58%)`;
}
function eAnySolo() {
  return eTracks.some((t) => t.removable && t.solo);
}
function eIsExcluded(t) {
  if (!t.removable) return false;
  if (eAnySolo()) return !t.solo;
  return !t.keep;
}
// Draws one app's peaks across the TIMELINE: peaks cover the main source, so
// each source-0 segment paints its slice of buckets into its ripple slot.
// Imported segments have no per-app data and stay empty.
function redrawTrackWaves() {
  const { starts, total } = eRipple();
  const srcDur0 = eSrcDur(0);
  const rows = $("e-tracks").querySelectorAll(".track-row");
  rows.forEach((row) => {
    const t = eTracks[Number(row.dataset.trackIndex)];
    const canvas = row.querySelector("canvas.wave");
    if (!t || !canvas) return;
    const ctx = canvas.getContext("2d");
    const W = canvas.width, H = canvas.height;
    ctx.clearRect(0, 0, W, H);
    const peaks = t.peaks || [];
    if (!(total > 0) || !(srcDur0 > 0) || !peaks.length) return;
    const muted = eIsExcluded(t);
    ctx.fillStyle = muted ? "#4a525d" : eTrackColor(t.pid);
    for (let k = 0; k < eClips.length; ++k) {
      const c = eClips[k];
      if ((c.src || 0) !== 0) continue;
      const x0 = Math.floor(starts[k] / total * W);
      const x1 = Math.floor((starts[k] + eSegOutMs(c)) / total * W);
      const span = Math.max(1, x1 - x0);
      const b0 = Math.floor(c.s / srcDur0 * peaks.length);
      const b1 = Math.max(b0 + 1, Math.ceil(c.e / srcDur0 * peaks.length));
      const nb = b1 - b0;
      for (let x = 0; x < span; ++x) {
        const b = Math.min(peaks.length - 1, b0 + Math.floor(x / span * nb));
        const v = peaks[b] || 0;
        const h = Math.max(1, Math.round(v * (H - 4)));
        ctx.fillRect(x0 + x, (H - h) / 2, 1, h);
      }
      // Segment boundary ticks keep cut positions legible over the wave.
      if (k > 0) {
        ctx.fillStyle = muted ? "rgba(255,255,255,.15)" : "rgba(255,255,255,.28)";
        ctx.fillRect(x0, 0, 1, H);
        ctx.fillStyle = muted ? "#4a525d" : eTrackColor(t.pid);
      }
    }
  });
}
function renderEditTracks(restoreFocus = null) {
  const wrap = $("e-tracks");
  const summary = $("e-audio-summary");
  const scrollPane = wrap.closest(".tracks-wrap");
  const scrollTop = scrollPane.scrollTop;
  const active = document.activeElement;
  if (!restoreFocus && wrap.contains(active)) {
    const row = active.closest(".track-row");
    if (active.id === "e-mixed-toggle") restoreFocus = { mixed: true };
    else if (row) {
      const control = active.matches("[data-solo]") ? "solo" :
        active.matches("[data-mute]") ? "mute" :
        active.matches("input.vol") ? "volume" : active.matches("canvas.wave") ? "wave" : null;
      if (control) restoreFocus = { index: Number(row.dataset.trackIndex), control };
    }
  }
  if (!eTracks.length) {
    wrap.innerHTML = `<div class="audio-state${eAudioReady ? "" : " loading"}">` +
      (eAudioReady
        ? "No separate audio layers were found. This clip will use its original mixed audio."
        : "Loading audio layers...") + "</div>";
    if (summary) summary.textContent = eAudioReady
      ? "Original mixed audio only"
      : "Checking recorded audio layers...";
    return;
  }
  const soloActive = eAnySolo();
  if (summary) {
    const editable = eTracks.filter((t) => t.removable).length;
    summary.textContent = soloActive
      ? `${editable} editable layer${editable === 1 ? "" : "s"} - solo is active`
      : `${editable} editable layer${editable === 1 ? "" : "s"}`;
  }
  wrap.innerHTML = "";
  // Reorder the presentation only; saved projects and native audio retain their indices.
  const indexed = eTracks.map((track, index) => ({ track, index }));
  const editable = indexed.filter(({ track }) => track.removable);
  const mixed = indexed.filter(({ track }) => !track.removable);
  let mixedSection = null;
  let mixedBody = null;
  if (mixed.length) {
    mixedSection = document.createElement("section");
    mixedSection.className = "mixed-audio";
    mixedSection.innerHTML =
      `<button id="e-mixed-toggle" class="mixed-audio-toggle" type="button" aria-expanded="${eShowMixedTracks}" aria-controls="e-mixed-tracks"><svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><path d="m9 5 7 7-7 7"/></svg><span>${mixed.length} layer${mixed.length === 1 ? "" : "s"} mixed into source</span></button>` +
      `<div id="e-mixed-tracks" class="${eShowMixedTracks ? "" : "hidden"}" role="group" aria-label="Audio mixed into source"><p class="mixed-audio-note">These sounds are already in the recording and cannot be adjusted separately.</p></div>`;
    mixedBody = mixedSection.querySelector("#e-mixed-tracks");
    mixedBody.inert = !eShowMixedTracks;
    mixedSection.querySelector("button").addEventListener("click", () => {
      eShowMixedTracks = !eShowMixedTracks;
      renderEditTracks({ mixed: true });
    });
  }
  const visible = [...editable, ...(eShowMixedTracks ? mixed : [])];
  visible.forEach(({ track: t, index: i }) => {
    if (t.gain === undefined) t.gain = 1;
    const excluded = eIsExcluded(t);
    const row = document.createElement("div");
    row.dataset.trackIndex = String(i);
    row.className = "track-row" + (excluded ? " muted" : "") +
      (soloActive ? " solo-active" : "");
    const trackName = t.exe || ("pid " + t.pid);
    const trackState = !t.removable ? "Mixed into source" :
      (t.solo ? "Soloed" : (excluded ? "Excluded by solo" : "Included"));
    row.innerHTML =
      `<div class="track-head">` +
      `<span class="track-dot" aria-hidden="true" style="background:${eTrackColor(t.pid)}"></span>` +
      `<span class="track-name" title="${esc(t.removable ? "Separate layer - mute, solo, or set its volume" : "No separate layer - this audio is baked into the mix")}">${esc(trackName)}</span>` +
      `<span class="track-state">${esc(trackState)}</span>` +
      `<div class="track-actions">` +
      `<button class="track-btn solo${t.solo ? " on" : ""}" data-solo="${i}" aria-pressed="${t.solo ? "true" : "false"}" aria-label="Solo ${esc(trackName)}" title="Solo: keep only this layer's sound"${t.removable ? "" : " disabled"}><span class="track-word">Solo</span><span class="track-short">S</span></button>` +
      `<button class="track-btn mute${t.keep ? "" : " on"}" data-mute="${i}" aria-pressed="${t.keep ? "false" : "true"}" aria-label="Mute ${esc(trackName)}" title="Mute this layer"${t.removable ? "" : " disabled"}><span class="track-word">Mute</span><span class="track-short">M</span></button>` +
      `</div></div>` +
      `<div class="track-controls">` +
      `<input type="range" class="vol" min="0" max="200" value="${Math.round(t.gain * 100)}" aria-label="${esc(trackName)} volume"${t.removable ? "" : " disabled"}>` +
      `<span class="volpct">${Math.round(t.gain * 100)}%</span>` +
      `</div>` +
      `<canvas class="wave" width="960" height="38" tabindex="0" role="slider" aria-label="${esc(trackName)} waveform. Use arrow keys to seek." aria-valuemin="0" aria-valuemax="${Math.max(0, eRipple().total / 1000).toFixed(1)}" aria-valuenow="0"></canvas>`;
    const seekWave = (event) => {
      const r = event.currentTarget.getBoundingClientRect();
      if (!(r.width > 0)) return;
      const { total } = eRipple();
      if (!(total > 0)) return;
      const tMs = Math.round(total * Math.min(1, Math.max(0, (event.clientX - r.left) / r.width)));
      const hit = eTimelineToSource(tMs);
      if (!hit || !eClips[hit.i]) return;
      if (hit.i !== eSel) selectSeg(hit.i, false);
      ensureVideoSrc(hit.i, Math.min(Math.max(hit.v, eClips[hit.i].s), eClips[hit.i].e));
    };
    row.querySelector("[data-mute]").addEventListener("click", () => {
      t.keep = !t.keep;
      renderEditTracks({ index: i, control: "mute" });
      ePushHist();
    });
    row.querySelector("[data-solo]").addEventListener("click", () => {
      t.solo = !t.solo;
      renderEditTracks({ index: i, control: "solo" });
      ePushHist();
    });
    const slider = row.querySelector("input.vol");
    const pct = row.querySelector(".volpct");
    slider.addEventListener("input", () => {
      t.gain = (parseInt(slider.value, 10) || 0) / 100;
      pct.textContent = slider.value + "%";
      if (t.gain <= 0 && t.keep) {
        t.keep = false;
        renderEditTracks();
      } else if (t.gain > 0 && !t.keep) {
        t.keep = true;
        renderEditTracks();
      }
    });
    // Commit gain once per gesture (input fires continuously while sliding).
    slider.addEventListener("change", () => ePushHist());
    const wave = row.querySelector("canvas.wave");
    wave.addEventListener("pointerup", seekWave);
    wave.addEventListener("keydown", (ev) => {
      const c = eClips[eSel];
      if (!c || !["ArrowLeft", "ArrowRight", "Home", "End"].includes(ev.key)) return;
      ev.preventDefault();
      const v = $("e-video");
      let next = v.currentTime || c.s;
      if (ev.key === "Home") next = c.s;
      else if (ev.key === "End") next = c.e;
      else next += ev.key === "ArrowLeft" ? -(ev.shiftKey ? 1 : 0.1) : (ev.shiftKey ? 1 : 0.1);
      ensureVideoSrc(eSel, Math.min(c.e, Math.max(c.s, next)));
    });
    (t.removable ? wrap : mixedBody).appendChild(row);
  });
  if (mixedSection) wrap.appendChild(mixedSection);
  redrawTrackWaves();
  scrollPane.scrollTop = scrollTop;
  if (restoreFocus) {
    const row = wrap.querySelector(`[data-track-index="${restoreFocus.index}"]`);
    const selector = { solo: "[data-solo]", mute: "[data-mute]", volume: "input.vol", wave: "canvas.wave" }[restoreFocus.control];
    const target = restoreFocus.mixed ? $("e-mixed-toggle") : row && selector ? row.querySelector(selector) : null;
    if (target && !target.disabled) focusVisible(target);
  }
}
function eUpdateTrackPlayhead() {
  const ph = $("e-track-ph");
  const c = eClips[eSel];
  const { starts, total } = eRipple();
  if (!c || !(total > 0) || eVideoSrcId !== eSegSourceId(eSel) ||
      $("e-tracks").querySelector(".track-row") === null) {
    ph.classList.add("hidden");
    return;
  }
  ph.classList.remove("hidden");
  const sp = c.spd || 1;
  const tMs = starts[eSel] + Math.round((($("e-video").currentTime || 0) - c.s) * 1000 / sp);
  ph.style.left = (tFrac(tMs, total) * 100) + "%";
}
function ePreviewTimelineSeconds() {
  const c = eClips[eSel];
  if (!c) return 0;
  const { starts } = eRipple();
  const sourceTime = Math.min(c.e, Math.max(c.s, $("e-video").currentTime || c.s));
  return (starts[eSel] + (sourceTime - c.s) * 1000 / (c.spd || 1)) / 1000;
}
function eSyncPreviewMode() {
  const c = eClips[eSel];
  const v = $("e-video");
  if (!c || !v) return;
  const speed = c.spd || 1;
  v.playbackRate = speed;
  if ($("e-preview-mode")) $("e-preview-mode").textContent = `Selected segment - ${speed}x preview`;
  if (!eBlurMode && $("e-editor-status")) {
    $("e-editor-status").textContent = "Previewing the selected segment. Edits are non-destructive.";
  }
}
function renderTextPreview() {
  const wrap = $("e-text-preview");
  if (!wrap) return;
  wrap.innerHTML = "";
  const at = ePreviewTimelineSeconds();
  const x = ["6%", "50%", "94%"];
  const y = ["10%", "50%", "90%"];
  const moveX = ["0", "-50%", "-100%"];
  const size = { S: "clamp(15px, 2vw, 23px)", M: "clamp(20px, 3.1vw, 38px)", L: "clamp(28px, 4.8vw, 58px)" };
  editTexts.forEach((t) => {
    if (at < t.start || at > t.end) return;
    const el = document.createElement("span");
    const px = [0, 1, 2].includes(t.px | 0) ? (t.px | 0) : 1;
    const py = [0, 1, 2].includes(t.py | 0) ? (t.py | 0) : 2;
    el.className = "preview-text";
    el.textContent = t.content || "";
    el.style.left = x[px];
    el.style.top = y[py];
    el.style.transform = `translate(${moveX[px]}, -50%)`;
    el.style.textAlign = t.align || "center";
    el.style.fontSize = size[t.size] || size.M;
    el.style.fontFamily = t.font || "Segoe UI";
    el.style.color = t.color || "#ffffff";
    wrap.appendChild(el);
  });
}
function openEditor() {
  const c = clips.find((x) => x.id === editId);
  if (!c) return;
  video.pause();
  setInspectorOpen(window.innerWidth > 800);
  selectInspectorPanel("video");
  if ($("e-clipname")) $("e-clipname").textContent = eClipLabel(editId) || c.id;
  editDurMs = c.dur_ms || 0;
  eDur = editDurMs / 1000;
  eIn = 0;
  eOut = eDur;
  eClips = [{ id: "c0", src: 0, s: 0, e: Math.round(eDur * 1000) / 1000 }];
  eSources = [{ id: editId, name: eClipLabel(editId) || "This clip", dur: eDur }];
  eVideoSrcId = editId;
  ePendingSeek = null;
  eRefreshImportList();
  eSeq = 0;
  eHist = [];
  eHistIx = -1;
  eFresh = true;
  eTracks = [];
  eShowMixedTracks = false;
  eAudioReady = false;
  clearEditorMediaNotice();
  $("e-film").innerHTML = '<div class="filmstrip-state">Loading preview frames...</div>';
  $("e-tracks").innerHTML = '<div class="audio-state loading">Loading audio layers...</div>';
  if ($("e-audio-summary")) $("e-audio-summary").textContent = "Checking recorded audio layers...";
  eSetSaveState("Preparing editor", "ready");
  if (video.src) $("e-video").src = video.src;
  else $("e-video").removeAttribute("src");
  $("e-start").value = "0";
  $("e-end").value = (editDurMs / 1000).toFixed(1);
  if ($("e-res")) $("e-res").value = "0";
  if ($("e-fps")) $("e-fps").value = "0";
  if ($("e-quality")) $("e-quality").value = "1";
  $("e-crop-preset").value = "full";
  if ($("e-canvas-fit")) $("e-canvas-fit").value = "0";
  syncCanvasRow();
  editRegions = [];
  editSel = -1;
  renderEditRegions();
  editTexts = [];
  textSel = -1;
  renderTextList();
  syncTextEditor();
  renderEditRuler();
  selectSeg(0, false);
  ePushHist(false);
  eSetSaveState("Preparing editor", "ready");
  eFresh = true;  // until the true duration arrives or the user edits
  post({ cmd: "editor_data", id: editId });
  setBlurMode(false);
  syncPreviewOverlay();
  $("e-progress-wrap").classList.add("hidden");
  $("e-progress-bar").style.transform = "scaleX(0)";
  $("e-render-summary").textContent = "Save creates a new clip. Your original stays intact.";
  showDialog(editOv, $("e-play"), overlay);
  eUpdateEditorChrome();
}
function closeEditor(flush = true) {
  try { $("e-video").pause(); } catch (e) {}
  if (flush) eFlushAutosave();
  else clearTimeout(eSaveTimer);
  setBlurMode(false);
  hideDialog(editOv);
  eDrag = null;
}

/* ---------------- Video overlay (blur boxes on the picture) ---------------- */
// The overlay is sized to the video's CONTENT box (object-fit contain math)
// so drawn boxes land on exact frame fractions regardless of letterboxing.
function syncPreviewOverlay() {
  const v = $("e-video"), ov = $("e-preview");
  if (!v || !ov) return;
  const vw = v.videoWidth || 0, vh = v.videoHeight || 0;
  const ew = v.clientWidth || 0, eh = v.clientHeight || 0;
  if (!(vw > 0 && vh > 0 && ew > 0 && eh > 0)) {
    ov.style.width = "0px";
    ov.style.height = "0px";
    return;
  }
  const s = Math.min(ew / vw, eh / vh);
  const cw = vw * s, ch = vh * s;
  ov.style.width = cw + "px";
  ov.style.height = ch + "px";
  ov.style.left = (v.offsetLeft + (ew - cw) / 2) + "px";
  ov.style.top = (v.offsetTop + (eh - ch) / 2) + "px";
}
window.addEventListener("resize", () => syncPreviewOverlay());
$("e-video").addEventListener("loadedmetadata", () => syncPreviewOverlay());
if (window.ResizeObserver) {
  new ResizeObserver(() => syncPreviewOverlay()).observe($("e-video"));
}
let eBlurMode = false;
function setBlurMode(on) {
  eBlurMode = !!on;
  if (eBlurMode) selectInspectorPanel("video");
  const btn = $("e-blur-mode"), ov = $("e-preview");
  if (btn) {
    btn.setAttribute("aria-pressed", eBlurMode ? "true" : "false");
    btn.textContent = "Blur tool: " + (eBlurMode ? "on" : "off");
  }
  if (ov) ov.classList.toggle("armed", eBlurMode);
  if ($("e-editor-status")) {
    $("e-editor-status").textContent = eBlurMode
      ? "Drag on the preview to draw a blur box. Drag a box to move it."
      : "Previewing the selected segment. Edits are non-destructive.";
  }
}
$("e-blur-mode").addEventListener("click", () => setBlurMode(!eBlurMode));

/* ---------------- Transport ---------------- */
function eClock(s) {
  s = Math.max(0, s || 0);
  const m = Math.floor(s / 60);
  const r = s - m * 60;
  return m + ":" + (r < 10 ? "0" : "") + r.toFixed(1);
}
function eUpdateClock() {
  const v = $("e-video"), out = $("e-clock");
  if (!v || !out) return;
  const c = eClips[eSel];
  const start = c ? c.s : 0;
  const end = c ? c.e : (v.duration || 0);
  out.textContent = eClock(Math.max(0, (v.currentTime || 0) - start)) + " / " +
    eClock(Math.max(0, end - start));
  renderTextPreview();
}
function clearEditorMediaNotice() {
  const notice = $("e-media-notice");
  const hadFocus = notice.contains(document.activeElement);
  notice.classList.add("hidden");
  if (hadFocus) focusVisible($("e-play"));
}
function handleEditorPlayFailure(error) {
  if (editOv.classList.contains("hidden") || error?.name === "AbortError") return;
  $("e-media-detail").textContent = error?.name === "NotAllowedError"
    ? "Playback paused. Choose Retry preview to start it."
    : "Preview could not play. Retry, or close the editor and reopen the clip.";
  $("e-media-notice").classList.remove("hidden");
}
function playEditorPreview() {
  const v = $("e-video");
  const c = eClips[eSel];
  try {
    if (c) {
      v.playbackRate = c.spd || 1;
      if (v.readyState >= 1 && (v.currentTime < c.s || v.currentTime >= c.e)) v.currentTime = c.s;
    }
    v.play().catch(handleEditorPlayFailure);
  } catch (error) {
    handleEditorPlayFailure(error);
  }
}
$("e-play").addEventListener("click", () => {
  const v = $("e-video");
  if (v.paused) playEditorPreview();
  else v.pause();
});
$("e-media-retry").addEventListener("click", () => {
  const v = $("e-video");
  if (v.error) {
    const sourceId = eVideoSrcId;
    const wantedTime = v.currentTime;
    v.addEventListener("loadedmetadata", () => {
      if (editOv.classList.contains("hidden") || eVideoSrcId !== sourceId) return;
      const c = eClips[eSel];
      if (c) v.currentTime = Math.min(c.e, Math.max(c.s, wantedTime));
    }, { once: true });
    v.load();
  }
  playEditorPreview();
});
$("e-video").addEventListener("error", () => handleEditorPlayFailure($("e-video").error));
$("e-video").addEventListener("loadstart", clearEditorMediaNotice);
$("e-video").addEventListener("playing", clearEditorMediaNotice);
$("e-video").addEventListener("play", () => {
  $("e-play").textContent = "Pause";
  $("e-play").setAttribute("aria-label", "Pause selected segment");
});
$("e-video").addEventListener("pause", () => {
  $("e-play").textContent = "Play";
  $("e-play").setAttribute("aria-label", "Play selected segment");
});
$("e-video").addEventListener("timeupdate", eUpdateClock);
$("e-video").addEventListener("loadedmetadata", eUpdateClock);
function eNudgePreview(delta) {
  const c = eClips[eSel];
  if (!c) return;
  const v = $("e-video");
  const next = Math.min(c.e, Math.max(c.s, (v.currentTime || c.s) + delta));
  ensureVideoSrc(eSel, next);
}

$("e-in").addEventListener("pointerdown", (e) => {
  eDrag = "in";
  eFresh = false;
  e.target.setPointerCapture(e.pointerId);
  e.preventDefault();
});
$("e-out").addEventListener("pointerdown", (e) => {
  eDrag = "out";
  eFresh = false;
  e.target.setPointerCapture(e.pointerId);
  e.preventDefault();
});
function eNudgeTrim(which, ev) {
  if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(ev.key)) return;
  const c = eClips[eSel];
  if (!c) return;
  ev.preventDefault();
  const step = ev.shiftKey ? 1 : 0.1;
  if (ev.key === "Home") {
    if (which === "in") eIn = 0; else eOut = c.s + 0.1;
  } else if (ev.key === "End") {
    if (which === "in") eIn = c.e - 0.1; else eOut = eSrcDur(c.src || 0);
  } else if (which === "in") {
    eIn += ev.key === "ArrowLeft" ? -step : step;
  } else {
    eOut += ev.key === "ArrowLeft" ? -step : step;
  }
  c.s = Math.round(eIn * 1000) / 1000;
  c.e = Math.round(eOut * 1000) / 1000;
  syncEditTimeline();
  ePushHist();
}
$("e-in").addEventListener("keydown", (e) => eNudgeTrim("in", e));
$("e-out").addEventListener("keydown", (e) => eNudgeTrim("out", e));
$("e-track").addEventListener("pointermove", (e) => {
  if (!eDrag) return;
  const c = eClips[eSel];
  if (!c) return;
  // Timeline integer-ms -> selected segment's source seconds.
  const { starts } = eRipple();
  const sp = c.spd || 1;
  const v = c.s + (eSeekEvt(e) - starts[eSel]) / 1000 * sp;
  if (eDrag === "in") eIn = v; else eOut = v;
  syncEditTimeline();
});
$("e-track").addEventListener("pointerup", (e) => {
  if (eDrag) {
    const was = eDrag;
    eDrag = null;
    // Commit the drag into the selected segment (trim), one history entry.
    if (eClips[eSel] && (was === "in" || was === "out")) {
      eClips[eSel].s = Math.round(eIn * 1000) / 1000;
      eClips[eSel].e = Math.round(eOut * 1000) / 1000;
      renderSegBlocks();
      ePushHist();
    }
    return;
  }
  const hit = eTimelineToSource(eSeekEvt(e));
  if (hit && eClips[hit.i]) {
    if (hit.i !== eSel) selectSeg(hit.i, false);
    ensureVideoSrc(hit.i, Math.min(Math.max(hit.v, eClips[hit.i].s), eClips[hit.i].e));
    renderSegBlocks();
  }
});
$("e-track").addEventListener("pointercancel", () => { eDrag = null; });
$("e-start").addEventListener("change", () => {
  eIn = parseFloat($("e-start").value) || 0;
  syncEditTimeline();
  if (eClips[eSel]) {
    eClips[eSel].s = Math.round(eIn * 1000) / 1000;
    eClips[eSel].e = Math.round(eOut * 1000) / 1000;
    renderSegBlocks();
  }
  eFresh = false;
  ePushHist();
});
$("e-end").addEventListener("change", () => {
  eOut = parseFloat($("e-end").value) || 0;
  syncEditTimeline();
  if (eClips[eSel]) {
    eClips[eSel].s = Math.round(eIn * 1000) / 1000;
    eClips[eSel].e = Math.round(eOut * 1000) / 1000;
    renderSegBlocks();
  }
  eFresh = false;
  ePushHist();
});
$("e-video").addEventListener("timeupdate", () => {
  const ph = $("e-playhead");
  // Map the preview clock (selected segment's source time) onto the ripple.
  const c = eClips[eSel];
  const { starts, total } = eRipple();
  if (!c || !(total > 0) || eVideoSrcId !== eSegSourceId(eSel)) {
    ph.classList.add("hidden");
    return;
  }
  const v = $("e-video");
  if (v.currentTime >= c.e) {
    if (v.currentTime > c.e) v.currentTime = c.e;
    if (!v.paused) v.pause();
  }
  ph.classList.remove("hidden");
  const sp = c.spd || 1;
  const tMs = starts[eSel] + Math.round(((v.currentTime || 0) - c.s) * 1000 / sp);
  ph.style.left = (tFrac(tMs, total) * 100) + "%";
  eUpdateTrackPlayhead();
  $("e-tracks").querySelectorAll("canvas.wave").forEach((wave) => {
    wave.setAttribute("aria-valuenow", ePreviewTimelineSeconds().toFixed(1));
  });
});
function syncCanvasRow() {
  const p = $("e-crop-preset").value;
  $("e-custom-row").classList.toggle("hidden", p !== "custom");
  $("e-canvas-row").classList.toggle("hidden",
    !(p === "16:9" || p === "1:1" || p === "9:16" || p === "4:5" || p === "4:3"));
}
$("e-crop-preset").addEventListener("change", () => {
  syncCanvasRow();
  ePushHist();
});
["e-cx", "e-cy", "e-cw", "e-chh"].forEach((id) => {
  $(id).addEventListener("change", () => ePushHist());
});
$("e-canvas-fit").addEventListener("change", () => ePushHist());

$("e-add-blur").addEventListener("click", () => {
  editRegions.push({ x: 30, y: 30, w: 40, h: 20 });   // percent of frame
  editSel = editRegions.length - 1;
  renderEditRegions();
  ePushHist();
});

function renderEditRegions() {
  const wrap = $("e-regions");
  wrap.innerHTML = "";
  const list = $("e-blur-list");
  list.innerHTML = editRegions.length ? "" :
    '<div class="blur-chip">No blur boxes yet — drag on the picture to draw one.</div>';
  editRegions.forEach((r, i) => {
    const d = document.createElement("div");
    d.className = "region" + (i === editSel ? " sel" : "");
    d.style.left = r.x + "%"; d.style.top = r.y + "%";
    d.style.width = r.w + "%"; d.style.height = r.h + "%";
    d.addEventListener("mousedown", (ev) => {
      if (ev.target.classList.contains("rhandle")) return;
      ev.stopPropagation(); ev.preventDefault();
      const rect = $("e-preview").getBoundingClientRect();
      editDrag = { i, sx: ev.clientX, sy: ev.clientY, ox: r.x, oy: r.y, rect };
      editSel = i; renderEditRegions();
    });
    if (i === editSel) {
      const h = document.createElement("span");
      h.className = "rhandle";
      h.title = "Drag to resize";
      h.addEventListener("mousedown", (ev) => {
        ev.stopPropagation(); ev.preventDefault();
        editResize = { i, sx: ev.clientX, sy: ev.clientY, ow: r.w, oh: r.h,
                       rect: $("e-preview").getBoundingClientRect() };
      });
      d.appendChild(h);
    }
    wrap.appendChild(d);

    const chip = document.createElement("div");
    chip.className = "blur-chip" + (i === editSel ? " sel" : "");
    chip.setAttribute("role", "button");
    chip.setAttribute("tabindex", "0");
    chip.setAttribute("aria-pressed", i === editSel ? "true" : "false");
    chip.setAttribute("aria-label", "Select blur box " + (i + 1));
    chip.innerHTML =
      `<span>Box ${i + 1} - ${Math.round(r.x)},${Math.round(r.y)} · ${Math.round(r.w)}×${Math.round(r.h)}%</span><button title="Remove blur box" aria-label="Remove blur box ${i + 1}">${editorRemoveIcon}</button>`;
    chip.querySelector("button").addEventListener("click", (ev) => {
      ev.stopPropagation();
      editRegions.splice(i, 1); editSel = -1; renderEditRegions();
      ePushHist();
    });
    chip.addEventListener("click", (ev) => {
      if (ev.target.tagName === "BUTTON") return;
      editSel = i;
      renderEditRegions();
    });
    chip.addEventListener("keydown", (ev) => {
      if (ev.target.closest("button")) return;
      if (ev.key !== "Enter" && ev.key !== " ") return;
      ev.preventDefault();
      editSel = i;
      renderEditRegions();
    });
    list.appendChild(chip);
  });
  if (eDrawEl) wrap.appendChild(eDrawEl);
}
let editDrag = null;
let editResize = null;
let eDrawEl = null;
let eDraw = null;
function ePct(ev, rect) {
  return {
    x: Math.min(100, Math.max(0, ((ev.clientX - rect.left) / rect.width) * 100)),
    y: Math.min(100, Math.max(0, ((ev.clientY - rect.top) / rect.height) * 100))
  };
}
document.addEventListener("mousemove", (ev) => {
  if (editResize) {
    const r = editRegions[editResize.i];
    if (!r) return;
    const dxPct = ((ev.clientX - editResize.sx) / editResize.rect.width) * 100;
    const dyPct = ((ev.clientY - editResize.sy) / editResize.rect.height) * 100;
    r.w = Math.round(Math.max(4, Math.min(100 - r.x, editResize.ow + dxPct)));
    r.h = Math.round(Math.max(4, Math.min(100 - r.y, editResize.oh + dyPct)));
    renderEditRegions();
    return;
  }
  if (editDrag) {
    const r = editRegions[editDrag.i];
    if (!r) return;
    const dxPct = ((ev.clientX - editDrag.sx) / editDrag.rect.width) * 100;
    const dyPct = ((ev.clientY - editDrag.sy) / editDrag.rect.height) * 100;
    r.x = Math.max(0, Math.min(100 - r.w, editDrag.ox + dxPct));
    r.y = Math.max(0, Math.min(100 - r.h, editDrag.oy + dyPct));
    renderRegionPos(editDrag.i);
  }
});
document.addEventListener("mouseup", () => {
  if (editResize) {
    editResize = null;
    renderEditRegions();
    ePushHist();
  } else if (editDrag) {
    editDrag = null;
    ePushHist();  // commit the blur drag as one history entry
  }
});
function renderRegionPos(i) {
  const d = $("e-regions").children[i];
  if (!d) return;
  d.style.left = editRegions[i].x + "%"; d.style.top = editRegions[i].y + "%";
}

// Drag on the picture to draw a blur box (replaces click-to-add).
$("e-preview").addEventListener("pointerdown", (e) => {
  if (e.button !== 0) return;
  if (e.target.closest(".region")) return;  // moving/resizing an existing box
  const rect = $("e-preview").getBoundingClientRect();
  if (!(rect.width > 0)) return;
  const p = ePct(e, rect);
  eDraw = { x0: p.x, y0: p.y, rect };
  if (!eDrawEl) {
    eDrawEl = document.createElement("div");
    eDrawEl.className = "region draw";
  }
  eDrawEl.style.display = "none";
  $("e-regions").appendChild(eDrawEl);
  $("e-preview").setPointerCapture(e.pointerId);
});
$("e-preview").addEventListener("pointermove", (e) => {
  if (!eDraw || !eDrawEl) return;
  const p = ePct(e, eDraw.rect);
  const x = Math.min(eDraw.x0, p.x), y = Math.min(eDraw.y0, p.y);
  const w = Math.abs(p.x - eDraw.x0), h = Math.abs(p.y - eDraw.y0);
  eDrawEl.style.display = "block";
  eDrawEl.style.left = x + "%"; eDrawEl.style.top = y + "%";
  eDrawEl.style.width = w + "%"; eDrawEl.style.height = h + "%";
});
$("e-preview").addEventListener("pointerup", (e) => {
  if (!eDraw || !eDrawEl) return;
  const p = ePct(e, eDraw.rect);
  const x = Math.round(Math.min(eDraw.x0, p.x));
  const y = Math.round(Math.min(eDraw.y0, p.y));
  const w = Math.round(Math.abs(p.x - eDraw.x0));
  const h = Math.round(Math.abs(p.y - eDraw.y0));
  eDrawEl.style.display = "none";
  eDraw = null;
  if (w >= 5 && h >= 5) {
    editRegions.push({ x, y, w: Math.min(w, 100 - x), h: Math.min(h, 100 - y) });
    editSel = editRegions.length - 1;
    renderEditRegions();
    ePushHist();
  }
});

function collectCrop(srcW, srcH) {
  const p = $("e-crop-preset").value;
  if (p === "custom") {
    return { x: parseInt($("e-cx").value) || 0, y: parseInt($("e-cy").value) || 0,
             w: parseInt($("e-cw").value) || 0, h: parseInt($("e-chh").value) || 0 };
  }
  if (p === "full") return { x: 0, y: 0, w: 0, h: 0 };
  const target = { "16:9": 16 / 9, "1:1": 1, "9:16": 9 / 16, "4:5": 4 / 5 }[p];
  let w = srcW, h = Math.round(w / target);
  if (h > srcH) { h = srcH; w = Math.round(h * target); }
  return { x: Math.round((srcW - w) / 2), y: Math.round((srcH - h) / 2), w, h };
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

["e-res", "e-fps", "e-quality"].forEach((id) => {
  const el = $(id);
  if (el) el.addEventListener("change", () => eScheduleAutosave());
});
$("e-speed").addEventListener("change", () => {
  if (!eClips[eSel]) return;
  eClips[eSel].spd = parseFloat($("e-speed").value) || 1;
  renderSegBlocks();
  eSyncPreviewMode();
  ePushHist();
});
$("e-split").addEventListener("click", eSplitAtPlayhead);
$("e-delete").addEventListener("click", eDeleteSel);
$("e-move-l").addEventListener("click", () => eMoveSel(-1));
$("e-move-r").addEventListener("click", () => eMoveSel(1));
$("e-undo").addEventListener("click", eUndo);
$("e-redo").addEventListener("click", eRedo);
$("e-reset").addEventListener("click", () => {
  if (!editId || !(eDur > 0)) return;
  eClips = [{ id: "c0", src: 0, s: 0, e: Math.round(eDur * 1000) / 1000 }];
  eSources = [{ id: editId, name: eClipLabel(editId) || "This clip", dur: eDur }];
  eVideoSrcId = editId;
  eRefreshImportList();
  eSel = 0;
  editRegions = [];
  editSel = -1;
  editTexts = [];
  textSel = -1;
  renderTextList();
  syncTextEditor();
  if ($("e-crop-preset")) $("e-crop-preset").value = "full";
  if ($("e-canvas-fit")) $("e-canvas-fit").value = "0";
  syncCanvasRow();
  if ($("e-res")) $("e-res").value = "0";
  if ($("e-fps")) $("e-fps").value = "0";
  if ($("e-quality")) $("e-quality").value = "1";
  ["e-cx", "e-cy", "e-cw", "e-chh"].forEach((id) => { if ($(id)) $(id).value = ""; });
  eTracks.forEach((t) => { t.keep = true; t.gain = 1; t.solo = false; });
  renderEditRegions();
  renderEditTracks();
  selectSeg(0, false);
  post({ cmd: "project_clear", id: editId });
  ePushHist();
  showToast("Editor reset");
});
document.addEventListener("keydown", (e) => {
  if ($("edit-overlay").classList.contains("hidden")) return;
  const tag = (e.target && e.target.tagName) || "";
  // Focused buttons keep their native Space/Enter activation; every other
  // editor key would double-fire with it, so stand down (Esc still closes).
  if (tag === "BUTTON") return;
  if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") {
    if (e.key !== "Escape") return;
  }
  const mod = e.ctrlKey || e.metaKey;
  if (mod && e.key.toLowerCase() === "z" && !e.shiftKey) { e.preventDefault(); eUndo(); return; }
  if ((mod && e.key.toLowerCase() === "y") || (mod && e.shiftKey && e.key.toLowerCase() === "z")) {
    e.preventDefault(); eRedo(); return;
  }
  if (mod) return;
  if (e.key === " ") { e.preventDefault(); const v = $("e-video"); v.paused ? playEditorPreview() : v.pause(); }
  else if (e.key === "s" || e.key === "S") eSplitAtPlayhead();
  else if (e.key === "Delete" || e.key === "Backspace") { e.preventDefault(); eDeleteSel(); }
  else if (e.key === "ArrowLeft") eNudgePreview(-(e.shiftKey ? 1 / 30 : 5));
  else if (e.key === "ArrowRight") eNudgePreview(e.shiftKey ? 1 / 30 : 5);
});

$("edit-render").addEventListener("click", () => {
  if (!editId) return;
  const blurs = editRegions.map((r) =>
    `${Math.round(r.x)},${Math.round(r.y)},${Math.round(r.w)},${Math.round(r.h)}`).join(";");
  // Solo wins over mute: any soloed app keeps its sound, all other
  // removable apps are excluded regardless of their keep flag.
  const soloOn = eTracks.some((t) => t.removable && t.solo);
  const excluded = eTracks
    .filter((t) => t.removable && (soloOn ? !t.solo : !t.keep))
    .map((t) => t.pid).join(",");
  const gains = eTracks
    .filter((t) => t.removable && (soloOn ? t.solo : t.keep) &&
                   Math.abs((t.gain === undefined ? 1 : t.gain) - 1) > 0.005)
    .map((t) => t.pid + ":" + (t.gain === undefined ? 1 : t.gain).toFixed(2)).join(",");
  const preset = $("e-crop-preset").value;
  const fitOn = ["16:9", "1:1", "9:16", "4:5", "4:3"].includes(preset) &&
    $("e-canvas-fit") && $("e-canvas-fit").value === "1";
  const crop = {
    crop: preset,
    canvas_fit: fitOn ? 1 : 0,
    cx: parseInt($("e-cx") && $("e-cx").value) || 0,
    cy: parseInt($("e-cy") && $("e-cy").value) || 0,
    cw: parseInt($("e-cw") && $("e-cw").value) || 0,
    ch: parseInt($("e-chh") && $("e-chh").value) || 0,
    blurs, excluded, gains
  };
  const speeds = eClips.map((c) => c.spd || 1).join(";");
  const exp = {
    res: parseInt($("e-res") ? $("e-res").value : 0) || 0,
    fps: parseInt($("e-fps") ? $("e-fps").value : 0) || 0,
    quality: $("e-quality") ? (parseInt($("e-quality").value) || 0) : 1
  };
  const customExp = exp.res > 0 || exp.fps > 0 || exp.quality !== 1;
  const texts = editTexts.map((t) =>
    [t.content, t.px, t.py, t.size, t.color, t.align,
     Math.round(t.start * 1000), Math.round(t.end * 1000), t.font].join("")).join("");
  const multi = eClips.length > 1 || (eClips.length && (eClips[0].spd || 1) !== 1) ||
    fitOn || customExp || editTexts.length > 0 ||
    (eClips.length === 1 && (eClips[0].src || 0) !== 0);
  if (multi) {
    const segments = eClips
      .map((c) => (c.src || 0) + "," + Math.round(c.s * 1000) + "," + Math.round(c.e * 1000)).join(";");
    const sources = eSources.slice(1).map((s) => s.id).join(";");
    post(Object.assign({ cmd: "project_render", id: editId, segments, speeds, sources,
      texts }, exp, crop));
  } else {
    post(Object.assign({
      cmd: "edit_render",
      id: editId,
      start_s: eClips.length ? eClips[0].s : (parseFloat($("e-start").value) || 0),
      end_s: eClips.length ? eClips[0].e : (parseFloat($("e-end").value) || 0)
    }, crop));
  }
  eFlushAutosave();
  closeEditor(false);
  showToast("Rendering started…");
});
/* ---------------- Text overlays ---------------- */
let editTexts = [];
let textSel = -1;

function renderTextList() {
  const list = $("e-text-list");
  list.innerHTML = editTexts.length ? "" :
    '<div class="blur-chip">No text — add one, then pick position and timing.</div>';
  editTexts.forEach((t, i) => {
    const chip = document.createElement("div");
    chip.className = "blur-chip" + (i === textSel ? " sel" : "");
    chip.setAttribute("role", "button");
    chip.setAttribute("tabindex", "0");
    chip.setAttribute("aria-pressed", i === textSel ? "true" : "false");
    chip.setAttribute("aria-label", "Select text overlay " + (i + 1));
    chip.innerHTML =
      `<span>${esc(t.content || "(empty)")} · ${eFmt(t.start)}–${eFmt(t.end)}</span><button title="Remove">${editorRemoveIcon}</button>`;
    chip.querySelector("button").setAttribute("aria-label", "Remove text overlay " + (i + 1));
    chip.querySelector("button").addEventListener("click", (ev) => {
      ev.stopPropagation();
      editTexts.splice(i, 1);
      if (textSel >= editTexts.length) textSel = editTexts.length - 1;
      syncTextEditor();
      ePushHist();
    });
    chip.addEventListener("click", () => { textSel = i; syncTextEditor(); });
    chip.addEventListener("keydown", (ev) => {
      if (ev.target.closest("button")) return;
      if (ev.key !== "Enter" && ev.key !== " ") return;
      ev.preventDefault();
      textSel = i;
      syncTextEditor();
    });
    list.appendChild(chip);
  });
  renderTextPreview();
}
function syncTextEditor() {
  const t = editTexts[textSel];
  $("e-text-edit").classList.toggle("hidden", !t);
  if (!t) {
    renderTextPreview();
    return;
  }
  $("e-text-content").value = t.content;
  $("e-text-size").value = t.size;
  $("e-text-color").value = t.color;
  $("e-text-font").value = t.font;
  $("e-text-align").value = t.align;
  $("e-text-start").value = t.start.toFixed(1);
  $("e-text-end").value = t.end.toFixed(1);
  [...$("e-text-pos").querySelectorAll("button")].forEach((b) => {
    const active = parseInt(b.dataset.px, 10) === t.px && parseInt(b.dataset.py, 10) === t.py;
    b.classList.toggle("on", active);
    b.setAttribute("aria-pressed", active ? "true" : "false");
  });
  renderTextPreview();
}
(function buildTextPosGrid() {
  const g = $("e-text-pos");
  for (let py = 0; py < 3; ++py) {
    for (let px = 0; px < 3; ++px) {
      const b = document.createElement("button");
      b.dataset.px = px;
      b.dataset.py = py;
      b.title = ["Top ", "", "Bottom "][py] + ["left", "center", "right"][px];
      b.setAttribute("aria-label", b.title + " text position");
      b.setAttribute("aria-pressed", "false");
      b.innerHTML = `<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><rect x="3" y="3" width="18" height="18" rx="2" opacity=".45"/><circle cx="${6 + px * 6}" cy="${6 + py * 6}" r="2" fill="currentColor" stroke="none"/></svg>`;
      b.addEventListener("click", () => {
        if (!editTexts[textSel]) return;
        editTexts[textSel].px = px;
        editTexts[textSel].py = py;
        syncTextEditor();
        ePushHist();
      });
      g.appendChild(b);
    }
  }
})();
$("e-add-text").addEventListener("click", () => {
  selectInspectorPanel("text");
  editTexts.push({
    content: "Title", size: "M", color: "#ffffff", font: "Segoe UI",
    px: 1, py: 2, align: "center",
    start: Math.round(eIn * 10) / 10,
    end: Math.round((eOut > eIn ? eOut : eDur) * 10) / 10
  });
  textSel = editTexts.length - 1;
  renderTextList();
  syncTextEditor();
  ePushHist();
});
[["e-text-content", "content", (v) => String(v || "").slice(0, 200)],
 ["e-text-size", "size", (v) => ["S", "M", "L"].includes(v) ? v : "M"],
 ["e-text-color", "color", (v) => /^#[0-9a-fA-F]{6}$/.test(v) ? v : "#ffffff"],
 ["e-text-font", "font", (v) => v],
 ["e-text-align", "align", (v) => ["left", "center", "right"].includes(v) ? v : "center"]
].forEach(([id, key, clean]) => {
  $(id).addEventListener("change", () => {
    if (!editTexts[textSel]) return;
    editTexts[textSel][key] = clean($(id).value);
    renderTextList();
    ePushHist();
  });
});
[["e-text-start", "start"], ["e-text-end", "end"]].forEach(([id, key]) => {
  $(id).addEventListener("change", () => {
    if (!editTexts[textSel]) return;
    const v = Math.min(Math.max(0, parseFloat($(id).value) || 0), Math.max(eDur, 0.1));
    editTexts[textSel][key] = Math.round(v * 10) / 10;
    renderTextList();
    syncTextEditor();
    ePushHist();
  });
});

$("edit-cancel").addEventListener("click", closeEditor);
$("e-progress-cancel").addEventListener("click", () => {
  post({ cmd: "cancel_render", id: editId });
});

/* ---------------- App audio (per-app stem removal) ---------------- */
const appAudioOv = $("appaudio-overlay");
let appAudioId = null;

function openAppAudio(id) {
  appAudioId = id;
  post({ cmd: "app_audio", id });
}

function closeAppAudio() {
  hideDialog(appAudioOv);
  appAudioId = null;
}

function renderAppAudio(id, apps) {
  if (id !== appAudioId) return;
  const list = $("appaudio-list");
  if (!apps.length) {
    list.innerHTML =
      '<div class="blur-chip">No app data for this clip — it was recorded before per-app tracking, or nothing made sound.</div>';
  } else {
    list.innerHTML = apps.map((a) => {
      const removable = !!a.removable;
      return `<div class="set-row"><label for="appaudio-app-${a.pid}" title="${esc(removable ? "Uncheck to remove this app\u2019s sound" : "No separate track was recorded for this app")}">${esc(a.exe || ("pid " + a.pid))}${removable ? "" : " (mixed)"}</label>` +
        `<input id="appaudio-app-${a.pid}" type="checkbox" data-pid="${a.pid}" ${removable ? "checked" : "checked disabled"}></div>`;
    }).join("");
  }
  showDialog(appAudioOv, list.querySelector("input:not(:disabled)") || $("appaudio-close"), overlay);
}

/* ---------------- Interactive trim ---------------- */
const trimOv = $("trim-overlay");
const trimVideo = $("trim-video");
const trimTrack = $("trim-track");
let trimId = null;
let trimDur = 0;
let trimIn = 0;
let trimOut = 0;
let trimLoop = true;
let trimDrag = null;

function trimFrac(t) {
  return trimDur > 0 ? Math.min(1, Math.max(0, t / trimDur)) : 0;
}
function trimFmt(t) {
  return (Math.round(t * 10) / 10).toFixed(1) + "s";
}
function syncTrimUI() {
  if (!(trimDur > 0)) { trimIn = 0; trimOut = 0; }
  trimIn = Math.min(Math.max(0, trimIn), Math.max(0, trimOut - 0.1));
  trimOut = Math.min(Math.max(trimIn + 0.1, trimOut), Math.max(trimDur, 0.1));
  if (!(trimOut > trimIn)) { trimIn = 0; trimOut = trimDur; }
  $("t-start").value = (Math.round(trimIn * 10) / 10).toFixed(1);
  $("t-end").value = (Math.round(trimOut * 10) / 10).toFixed(1);
  $("t-start-label").textContent = trimFmt(trimIn);
  $("t-end-label").textContent = trimFmt(trimOut);
  $("t-len-label").textContent = "→ " + trimFmt(Math.max(0, trimOut - trimIn)) + " selected";
  $("trim-region").style.left = (trimFrac(trimIn) * 100) + "%";
  $("trim-region").style.width = ((trimFrac(trimOut) - trimFrac(trimIn)) * 100) + "%";
  $("trim-in").style.left = (trimFrac(trimIn) * 100) + "%";
  $("trim-out").style.left = (trimFrac(trimOut) * 100) + "%";
}
function trimSeekEvt(e) {
  const r = trimTrack.getBoundingClientRect();
  return trimDur * Math.min(1, Math.max(0, (e.clientX - r.left) / Math.max(1, r.width)));
}
function openTrim(id) {
  const c = clips.find((x) => x.id === id);
  if (!c) return;
  if (!video.src) { showToast("Play the clip first, then trim"); return; }
  trimId = id;
  trimDur = (c.dur_ms || 0) / 1000;
  trimIn = 0;
  trimOut = trimDur;
  trimVideo.src = video.src;
  syncTrimUI();
  showDialog(trimOv, $("t-start"), overlay);
  trimVideo.play().catch(() => {});
}
function closeTrim() {
  try { trimVideo.pause(); } catch (e) {}
  trimVideo.removeAttribute("src");
  try { trimVideo.load(); } catch (e) {}
  hideDialog(trimOv);
  trimId = null;
  trimDrag = null;
}
$("trim-close").addEventListener("click", closeTrim);
$("trim-cancel").addEventListener("click", closeTrim);
$("t-start").addEventListener("change", () => {
  trimIn = parseFloat($("t-start").value) || 0;
  syncTrimUI();
});
$("t-end").addEventListener("change", () => {
  trimOut = parseFloat($("t-end").value) || 0;
  syncTrimUI();
});
$("t-set-in").addEventListener("click", () => {
  trimIn = trimVideo.currentTime || 0;
  syncTrimUI();
});
$("t-set-out").addEventListener("click", () => {
  trimOut = trimVideo.currentTime || 0;
  syncTrimUI();
});
$("t-loop").addEventListener("click", () => {
  trimLoop = !trimLoop;
  $("t-loop").setAttribute("aria-pressed", trimLoop ? "true" : "false");
  $("t-loop").textContent = trimLoop ? "Loop: on" : "Loop: off";
});
$("trim-in").addEventListener("pointerdown", (e) => {
  trimDrag = "in";
  e.target.setPointerCapture(e.pointerId);
  e.preventDefault();
});
$("trim-out").addEventListener("pointerdown", (e) => {
  trimDrag = "out";
  e.target.setPointerCapture(e.pointerId);
  e.preventDefault();
});
trimTrack.addEventListener("pointermove", (e) => {
  if (!trimDrag) return;
  const t = trimSeekEvt(e);
  if (trimDrag === "in") trimIn = t; else trimOut = t;
  syncTrimUI();
});
trimTrack.addEventListener("pointerup", (e) => {
  if (trimDrag) { trimDrag = null; return; }
  trimVideo.currentTime = trimSeekEvt(e);
});
trimTrack.addEventListener("pointercancel", () => { trimDrag = null; });
trimVideo.addEventListener("timeupdate", () => {
  const ph = $("trim-playhead");
  if (!(trimDur > 0)) { ph.classList.add("hidden"); return; }
  ph.classList.remove("hidden");
  ph.style.left = (trimFrac(trimVideo.currentTime || 0) * 100) + "%";
  if (trimLoop && trimOut < trimDur - 0.05 && (trimVideo.currentTime || 0) >= trimOut) {
    trimVideo.currentTime = trimIn;
  }
});
$("trim-render").addEventListener("click", () => {
  if (!trimId) return;
  if (!(trimOut > trimIn)) {
    showToast("Drag the handles to pick a range first");
    return;
  }
  post({
    cmd: "trim_render",
    id: trimId,
    start_ms: Math.round(trimIn * 1000),
    end_ms: Math.round(trimOut * 1000)
  });
  closeTrim();
  showToast("Trimming…");
});

$("appaudio-close").addEventListener("click", closeAppAudio);
$("appaudio-cancel").addEventListener("click", closeAppAudio);
$("appaudio-render").addEventListener("click", () => {
  if (!appAudioId) return;
  const excluded = [...document.querySelectorAll("#appaudio-list input[data-pid]:not(:checked):not(:disabled)")]
    .map((i) => i.dataset.pid)
    .join(",");
  if (!excluded) {
    showToast("Uncheck an app to remove its sound");
    return;
  }
  post({ cmd: "app_audio_render", id: appAudioId, excluded });
  closeAppAudio();
  showToast("Rendering clean mix…");
});
$("edit-close").addEventListener("click", closeEditor);

showAppView("library");
setLibraryView("grid");
selectInspectorPanel("video");
updateCaptureSummary();
post({ cmd: "ready" });
post({ cmd: "get_settings" });
post({ cmd: "list_sources" });
