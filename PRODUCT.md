# ClipLite

<!-- impeccable:product-schema 1 -->

## Platform

web

The interface is local HTML, CSS, and JavaScript inside a native Windows C++20
application using WebView2. It is a desktop app; this record's `web` value describes
the interface technology, not a hosted website.

## Product Purpose

Keep a rolling screen and audio replay buffer, save recent footage on demand,
and make saved clips easy to find and play. These capabilities are documented
in README.md and implemented by the native message bridge.

## Operating Context

Windows 10/11 on an unlocked desktop. ClipLite runs in the system tray and exposes
a local clip library and video player, recording settings, capture source picker,
and microphone picker. Recordings and audio metadata stay on disk.

## Capabilities and Constraints

- Save recent footage with Clip Now or the configured global hotkey.
- Search and play the local library; rename, copy, reveal, or delete clips.
- Preserve the `window.chrome.webview.postMessage` / `window.__onNative` contract.
- Use local assets and Windows system fonts; no network dependency for the UI.
- Visual work must keep media preview geometry and recording behavior intact.

## Brand Commitments

The user requested a liquid glass redesign across the desktop interface and an
updated PC installation and GitHub source. The name ClipLite and its focus on recording and playback remain the product
identity.

On October 8, 2026, the user rejected the oversized blue frosted panels as generic
and explicitly chose **Apple-style clear liquid glass**. Navigation and control
chrome should transmit the content underneath, while recordings stay visually
quiet. This is a confirmed appearance preference.

## Evidence on Hand

README.md, assets/web/, src/ui/webview_window.cpp, src/ui/settings_window.cpp,
and the user's installed Windows application. Clip thumbnails are actual local
recordings supplied by the native host; the UI must not replace them with samples.

## Open Decisions

The user has not specified light or dark mode or a standing preference for
generated previews. The current build uses dark neutral working surfaces for
the existing desktop recording context. No standing build-path preference has
been recorded.
