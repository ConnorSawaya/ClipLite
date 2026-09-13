# ClipLite

ClipLite is a lightweight Windows desktop recorder for instant replays. It keeps a rolling buffer of your screen and audio in the background and saves the last N seconds on demand, with a built-in editor for trimming, cropping, blurring, text, timeline cuts, and per-app audio mixing.

> Native stack: C++20 / CMake / Ninja / MSVC, Win32 + WebView2, Media Foundation H.264/AAC. The UI is a local WebView, not a browser React site.

## Features

- **Instant replay buffer** — continuous desktop capture (DXGI Desktop Duplication) with a disk-backed ring of 10s MP4 segments.
- **One-key save** — `Clip Now` hotkey (default `F8`) saves the last 60s (configurable).
- **Game-aware naming** — executable + window title detection for readable clip names and thumbnails.
- **Audio that stays correct** — desktop loopback + microphone + per-process stems (Windows process loopback). Each app and the mic get their own layer, so muting/solo/volume in the editor never drops the voice.
- **Editor** — trim, split/delete/reorder segments, speed, crop/canvas (aspect presets + custom + fit/fill), blur boxes drawn on the preview, text overlays with position/size/color/font/alignment/timing, and export resolution/FPS/quality.
- **Library** — local clip grid, search, play, delete, and autosaved editor projects (`*.edit.json` next to each clip).
- **System integration** — tray icon, global hotkey, startup toggle, notifications, single-instance guard.

## Requirements

- Windows 10/11 x64 (process loopback needs build 20348+)
- MSVC 2022 Build Tools + Windows SDK + CMake 3.21+ + Ninja
- WebView2 Runtime (Evergreen)
- GPU with Media Foundation H.264 (hardware preferred, software fallback)
- Unlocked interactive desktop for capture; default audio endpoint for loopback; mic if mic capture is enabled

## Quick start

### Install from a local build

```powershell
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release'
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build build --config Release'
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --install build --config Release --prefix "%LOCALAPPDATA%\Programs\ClipLite"'
"%LOCALAPPDATA%\Programs\ClipLite\ClipLite.exe"
```

### CLI flags

```powershell
ClipLite.exe --version
ClipLite.exe --selftest
ClipLite.exe --library-selftest
ClipLite.exe --open
```

## Build and test

Configure once, then build and test:

```powershell
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release'
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build build && ctest --test-dir build --output-on-failure'
```

Direct unit suite (no capture hardware needed):

```powershell
.\build\cliplite_tests.exe
```

Optional AddressSanitizer build:

```powershell
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Release -DCLIPLITE_ASAN=ON
```

## Using the app

1. Launch `ClipLite.exe` — it sits in the tray and starts buffering.
2. Press the hotkey (`F8` by default) or click **Clip Now** to save.
3. Open a clip from the library grid (search supported), then **Edit**.
4. In the editor: select a timeline segment, drag trim handles, split at playhead, reorder, set speed, draw blur boxes on the preview, add text, choose crop/canvas, and adjust per-app audio (Mute/Solo + volume). Edits autosave to `<clip>.edit.json` and are non-destructive.
5. **Save new clip** renders a new file; the original stays intact. Render progress and errors are shown in the editor footer.

### Editor tips

- The preview shows the selected segment and respects its speed.
- Timeline clicks and waveform clicks seek the correct source segment.
- Solo wins over mute: any soloed layer keeps its sound, all other removable layers are excluded.
- Closing the editor flushes pending autosaves before leaving.

### Settings

Replay length, FPS, bitrate, quality, desktop/mic toggles, capture source (display/window), mic picker + level meter, per-game capture toggle, start-with-Windows, notifications, and hotkey. Stored at:

- `%LOCALAPPDATA%\ClipLite\settings.ini`

## Storage

- Clips: `%USERPROFILE%\Videos\ClipLite` (configurable via settings)
- Buffer (rolling segments): `%LOCALAPPDATA%\ClipLite\Buffer`
- Thumbnails / peaks cache: `%LOCALAPPDATA%\ClipLite\thumbs`
- Per-clip sidecars: `<clip>.apps.json`, `<clip>.edit.json`, `<clip>.stems/` (when per-app stems are available)

## Privacy

All capture and rendering happens locally on your machine. ClipLite does not upload clips to a cloud service. Clips are regular MP4 files on disk; delete them from the library or Explorer like any other file.

## Troubleshooting

- **No clips yet** — run with an unlocked desktop and default audio endpoint available; press the hotkey and check `%USERPROFILE%\Videos\ClipLite`.
- **WebView stays blank** — install/upgrade the WebView2 Evergreen Runtime and restart the app.
- **Mic silent** — check Windows Sound → Input, and the in-app mic picker/level meter.
- **File locked during test** — close any open file handle to the clip before deleting it; the app releases the file on `Finalize()`.
- **Locked test binary on rebuild** — a running `cliplite_dxgi_test.exe` or `ClipLite.exe` can hold the file; stop the process and rebuild.

## Project status

Active development. See `THIRD_PARTY_NOTICES.md` for attribution. No cloud auth, payments, or hosted database are included by design; the app uses mock/local data before any real crawling and respects system capture constraints.

## License

MIT — see `LICENSE`. Third-party notices in `THIRD_PARTY_NOTICES.md`. WebView2 loader binaries in `third_party/webview2/` are distributed under Microsoft's WebView2 terms.

## Contributing

Issues and pull requests are welcome. Keep changes focused, preserve the WebView message contract (`window.chrome.webview.postMessage` <-> `window.__onNative`), and include tests or a clear manual verification for editor/timeline/audio changes. Run `cmake --build build && ctest --test-dir build --output-on-failure` before submitting.
