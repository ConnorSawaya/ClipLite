# ClipLite

ClipLite is a lightweight Windows desktop recorder for instant replays. It keeps a rolling buffer of your screen and audio in the background, saves the last N seconds on demand, and gives you a simple local library for finding and playing clips.

> Native stack: C++20 / CMake / Ninja / MSVC, Win32 + WebView2, Media Foundation H.264/AAC. The UI is a local WebView, not a browser React site.

## Features

- **Instant replay buffer** — continuous desktop capture (DXGI Desktop Duplication) with a disk-backed ring of 10s MP4 segments.
- **One-key save** — `Clip Now` hotkey (default `F8`) saves the last 60s (configurable).
- **Game-aware naming** — executable + window title detection for readable clip names and thumbnails.
- **Audio capture** — record desktop and microphone audio with the devices you choose.
- **Library** — search, filter, sort, and play saved clips; rename, copy, reveal, or delete files.
- **Desktop interface** — a compact command bar with Capture, Library, and Settings workspaces.
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
3. Open **Library** to search or sort clips, then select one to play it. Use **More** for rename, copy, open-folder, and delete actions.

### Settings

Use **Capture** to choose a display/window, toggle PC or microphone audio, and choose a microphone. **Settings** separates Recording, Audio, Capture, and General controls, with Save settings available in every category. Replay length, FPS, bitrate, quality, per-game capture, startup, notifications, and the hotkey are stored at:

- `%LOCALAPPDATA%\ClipLite\settings.ini`

### Editing the interface

The local WebView assets are in `assets/web/`. Installation copies these files directly. When running `build/ClipLite.exe` after a web-only change that does not relink the app, refresh its asset directory:

```powershell
cmake -E copy_directory assets/web build/web
```

See `DESIGN.md` for the implemented visual system. System fonts and local SVG/CSS assets keep the interface independent of network services.

## Storage

- Clips: `%USERPROFILE%\Videos\ClipLite` (configurable via settings)
- Buffer (rolling segments): `%LOCALAPPDATA%\ClipLite\Buffer`
- Thumbnails / peaks cache: `%LOCALAPPDATA%\ClipLite\thumbs`
- Per-clip audio metadata: `<clip>.apps.json` and `<clip>.stems/` (when per-app stems are available)

## Privacy

All capture and rendering happens locally on your machine. ClipLite does not upload clips to a cloud service. Clips are regular MP4 files on disk; delete them from the library or Explorer like any other file.

## Troubleshooting

- **No clips yet** — run with an unlocked desktop and default audio endpoint available; press the hotkey and check `%USERPROFILE%\Videos\ClipLite`.
- **WebView stays blank** — install/upgrade the WebView2 Evergreen Runtime and restart the app.
- **Mic silent** — check Windows Sound → Input, and the in-app mic picker/level meter.
- **File locked during test** — close any open file handle to the clip before deleting it; the app releases the file on `Finalize()`.
- **Locked test binary on rebuild** — a running `cliplite_dxgi_test.exe` or `ClipLite.exe` can hold the file; stop the process and rebuild.

## Project status

Active development. Capture, playback, and rendering use local files and Windows media APIs. See `THIRD_PARTY_NOTICES.md` for attribution.

## License

MIT — see `LICENSE`. Third-party notices in `THIRD_PARTY_NOTICES.md`. WebView2 loader binaries in `third_party/webview2/` are distributed under Microsoft's WebView2 terms.

## Contributing

Issues and pull requests are welcome. Keep changes focused, preserve the WebView message contract (`window.chrome.webview.postMessage` <-> `window.__onNative`), and include a clear description of the change and its manual verification. Run `cmake --build build && ctest --test-dir build --output-on-failure` before submitting.
