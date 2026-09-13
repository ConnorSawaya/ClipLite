#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "cliplite/app/startup.h"
#include "cliplite/app/tray.h"
#include "cliplite/config.h"
#include "cliplite/detection/capture_selector.h"
#include "cliplite/detection/game_detector.h"
#include "cliplite/hotkeys.h"
#include "cliplite/log.h"
#include "cliplite/replay/recovery.h"
#include "cliplite/replay/replay_recorder.h"
#include "cliplite/ui/library_window.h"
#include "cliplite/ui/settings_window.h"
#include "cliplite/util/win_utf8.h"

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_CLIP_SAVED = WM_APP + 2;
constexpr UINT WM_APP_LOW_DISK = WM_APP + 3;
constexpr UINT WM_APP_SETTINGS_CHANGED = WM_APP + 4;
constexpr UINT WM_APP_CLIP_FAILED = WM_APP + 5;
constexpr UINT HOTKEY_SAVE_CLIP = 1;

constexpr UINT ID_TRAY_RECORDING = 1000;
constexpr UINT ID_TRAY_SAVE_CLIP = 1001;
constexpr UINT ID_TRAY_OPEN_LIBRARY = 1003;
constexpr UINT ID_TRAY_SETTINGS = 1004;
constexpr UINT ID_TRAY_EXIT = 1002;

constexpr const wchar_t* kWindowClassName = L"ClipLiteHiddenWindow";

HINSTANCE g_hInst = nullptr;
HWND g_hwnd = nullptr;
cliplite::app::TrayIcon g_tray;
UINT g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

std::unique_ptr<cliplite::replay::ReplayRecorder> g_recorder;
std::thread g_recorder_thread;
std::atomic<bool> g_recorder_stop{false};
std::atomic<bool> g_save_requested{false};
std::mutex g_clip_mutex;
std::wstring g_last_clip;
std::mutex g_recorder_mutex;
cliplite::detection::GameDetector g_detector;
cliplite::detection::CaptureSelector g_selector(30000);
std::string g_current_game;
std::string g_current_exe;
std::string g_last_reported_exe;
std::unique_ptr<cliplite::ui::LibraryWindow> g_library;
std::unique_ptr<cliplite::ui::SettingsWindow> g_settings_win;
cliplite::Settings g_settings;
uint32_t g_hotkey_mods = 0;
uint32_t g_hotkey_vk = VK_F8;

void console_write(const wchar_t* s) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteConsoleW(out, s, static_cast<DWORD>(wcslen(s)), &written, nullptr);
}

std::wstring env_or(const wchar_t* var, const wchar_t* fallback) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(fallback);
}

std::string w2u8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::wstring default_clip_dir() {
    return env_or(L"USERPROFILE", L"C:\\") + L"\\Videos\\ClipLite";
}

std::wstring default_buffer_dir() {
    return env_or(L"LOCALAPPDATA", L"C:\\Temp") + L"\\ClipLite\\Buffer";
}

std::string settings_path() {
    return w2u8(env_or(L"LOCALAPPDATA", L"C:\\Temp") + L"\\ClipLite\\settings.ini");
}

void notify_user(const std::wstring& title, const std::wstring& body);

// Recorder worker thread: owns the capture/encode/replay pipeline. Runs until
// g_recorder_stop is set; the F8/tray "Save Clip" action sets g_save_requested.
void recorder_thread_main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        CL_ERROR("App", "recorder thread COM init failed");
        return;
    }

    cliplite::replay::RecorderConfig cfg;
    cfg.fps = static_cast<uint32_t>(g_settings.recording.fps);
    cfg.video_bitrate_bps = static_cast<uint32_t>(g_settings.recording.bitrate_mbps) * 1'000'000;
    cfg.replay_ms = static_cast<uint32_t>(g_settings.recording.replay_duration_sec) * 1000;
    if (g_settings.recording.display_index >= 0)
        cfg.display_index = static_cast<uint32_t>(g_settings.recording.display_index);
    cfg.source_mode = g_settings.recording.source_mode;
    cfg.source_window_exe = g_settings.recording.source_window_exe;
    cfg.source_window_title = g_settings.recording.source_window_title;
    if (cfg.source_mode == "window") {
        // Isolated window capture (WGC backend) lands next: record the full
        // display meanwhile rather than failing. One reminder per session.
        static bool reminded = false;
        if (!reminded) {
            reminded = true;
            notify_user(L"Window capture is almost here",
                        L"Isolated per-window recording lands in the next update \u2014 "
                        L"recording the entire display for now.");
        }
        CL_INFO("App", "window source selected (" + cfg.source_window_exe +
                           ") - display fallback until WGC backend lands");
    }
    cfg.desktop_audio = g_settings.audio.desktop_enabled;
    cfg.mic_enabled = g_settings.audio.mic_enabled;
    cfg.mic_device = cliplite::util::utf8_to_wide(g_settings.audio.mic_device);
    // 10 s segments: replay trim precision is bounded by segment length, and
    // each rotation creates a fresh MF sink writer + hardware MFT, so shorter
    // segments multiply driver handle churn for no visible benefit.
    cfg.segment_ms = 10'000;
    cfg.buffer_dir = g_settings.storage.buffer_folder.empty()
                         ? default_buffer_dir()
                         : cliplite::util::utf8_to_wide(g_settings.storage.buffer_folder);
    cfg.clip_dir = g_settings.storage.clip_folder.empty()
                       ? default_clip_dir()
                       : cliplite::util::utf8_to_wide(g_settings.storage.clip_folder);
    std::error_code ec;
    std::filesystem::create_directories(cfg.buffer_dir, ec);
    std::filesystem::create_directories(cfg.clip_dir, ec);

    g_recorder = std::make_unique<cliplite::replay::ReplayRecorder>();
    if (!g_recorder->start(cfg)) {
        CL_ERROR("App", "recorder failed to start");
        g_recorder.reset();
        CoUninitialize();
        return;
    }
    CL_INFO("App", "replay recorder running");

    uint64_t last_disk_check = 0;
    while (!g_recorder_stop.load()) {
        if (!g_recorder->tick()) break;
        if (g_save_requested.exchange(false)) {
            const std::wstring clip = g_recorder->save_clip();
            if (!clip.empty()) {
                {
                    std::lock_guard<std::mutex> lock(g_clip_mutex);
                    g_last_clip = clip;
                }
                if (g_hwnd) PostMessageW(g_hwnd, WM_APP_CLIP_SAVED, 0, 0);
            } else if (g_hwnd) {
                // Give the user explicit feedback instead of failing silently.
                PostMessageW(g_hwnd, WM_APP_CLIP_FAILED, 0, 0);
            }
        }

        const uint64_t now = GetTickCount64();
        if (now - last_disk_check > 2000) {
            last_disk_check = now;
            const int64_t free_bytes = cliplite::replay::free_disk_bytes(cfg.buffer_dir);
            if (free_bytes >= 0 && free_bytes < 512LL * 1024 * 1024) {
                CL_WARN("App", "low disk space, pausing recording");
                if (g_hwnd) PostMessageW(g_hwnd, WM_APP_LOW_DISK, 0, 0);
                break;
            }
        }
        Sleep(8);
    }

    g_recorder->stop();
    {
        std::lock_guard<std::mutex> lock(g_recorder_mutex);
        g_recorder.reset();
    }
    CoUninitialize();
    CL_INFO("App", "recorder stopped");
}

void start_recorder() {
    if (g_recorder_thread.joinable()) return;
    g_recorder_stop.store(false);
    g_recorder_thread = std::thread(recorder_thread_main);
}

void stop_recorder() {
    g_recorder_stop.store(true);
    if (g_recorder_thread.joinable()) g_recorder_thread.join();
}

// All user-facing balloons go through here so Settings -> Notifications
// actually silences them (previously the toggle was saved but never read).
void notify_user(const std::wstring& title, const std::wstring& body) {

    if (!g_settings.general.notifications) return;
    g_tray.notify(title, body);
}

void trigger_save_clip() {
    if (g_recorder_thread.joinable()) {
        g_save_requested.store(true);
    } else {
        notify_user(L"Not recording right now",
                    L"No game detected at the moment - clips save the last 60s of detected games.");
        CL_INFO("App", "Save Clip requested but recorder is not running");
    }
}

void open_settings();
void open_library() {
    if (!g_library) {
        g_library = std::make_unique<cliplite::ui::LibraryWindow>();
        g_library->set_clip_request([] { trigger_save_clip(); });
        g_library->set_settings_request([] { open_settings(); });
        if (!g_library->create(g_hInst, default_clip_dir())) {
            CL_ERROR("App", "library window creation failed");
            g_library.reset();
            return;
        }
        g_library->attach_settings(&g_settings, settings_path(), g_hwnd,
                                   WM_APP_SETTINGS_CHANGED);
    }
    g_library->show();
}

void open_settings() {
    if (!g_settings_win) {
        // Settings now lives inside the web UI; keep the native window as a
        // fallback only if the library window failed to build.
        if (g_library) {
            g_library->open_settings_ui();
            return;
        }
        g_settings_win = std::make_unique<cliplite::ui::SettingsWindow>();
        if (!g_settings_win->create(g_hInst, &g_settings, settings_path(), g_hwnd,
                                    WM_APP_SETTINGS_CHANGED)) {
            CL_ERROR("App", "settings window creation failed");
            g_settings_win.reset();
            return;
        }
    } else if (g_library) {
        g_library->open_settings_ui();
        return;
    }
    g_settings_win->show();
}
void show_tray_menu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool recording = g_recorder_thread.joinable();
    std::wstring state = recording ? L"ClipLite \u2014 Recording" : L"ClipLite \u2014 Idle";
    if (recording && !g_current_game.empty())
        state += L" (" + cliplite::util::utf8_to_wide(g_current_game) + L")";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_TRAY_RECORDING, state.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_SAVE_CLIP, L"Save Clip");
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_LIBRARY, L"Open Clip Library");
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const std::string game = g_current_game.empty() ? "None" : g_current_game;
    const std::wstring line = L"Current Game: " + cliplite::util::utf8_to_wide(game);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0,
                   hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_TRAY: {
            const UINT event = LOWORD(lp);
            if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                show_tray_menu(hwnd);
            } else if (event == WM_LBUTTONDBLCLK) {
                open_library();
            }
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_TRAY_SAVE_CLIP:
                    trigger_save_clip();
                    break;
                case ID_TRAY_OPEN_LIBRARY:
                    open_library();
                    break;
                case ID_TRAY_SETTINGS:
                    open_settings();
                    break;
                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
                default:
                    break;
            }
            break;
        case WM_HOTKEY:
            if (wp == HOTKEY_SAVE_CLIP) trigger_save_clip();
            break;
        case WM_APP_CLIP_SAVED: {
            std::wstring clip;
            {
                std::lock_guard<std::mutex> lock(g_clip_mutex);
                clip = g_last_clip;
            }
            const auto slash = clip.find_last_of(L"\\/");
            const std::wstring name = (slash == std::wstring::npos) ? clip : clip.substr(slash + 1);
            notify_user(L"Clip saved", name);
            if (g_library) g_library->refresh_clips();
            CL_INFO("App", "clip saved notification shown");
            break;
        }
        case WM_APP_LOW_DISK:
            notify_user(L"ClipLite paused", L"Not enough free disk space for the replay buffer.");
            break;
        case WM_APP_CLIP_FAILED:
            notify_user(L"Clip not saved", L"Nothing recorded yet - capture has no replay to save.");
            CL_WARN("App", "save clip requested but the replay buffer had nothing");
            break;
        case WM_APP_SETTINGS_CHANGED:
            UnregisterHotKey(hwnd, HOTKEY_SAVE_CLIP);
            if (!g_settings.hotkeys.save_clip.empty()) {
                if (auto hk = cliplite::parse_hotkey(g_settings.hotkeys.save_clip)) {
                    g_hotkey_mods = hk->modifiers;
                    g_hotkey_vk = hk->vk;
                    RegisterHotKey(hwnd, HOTKEY_SAVE_CLIP, g_hotkey_mods, g_hotkey_vk);
                }
            }
            cliplite::app::set_startup_enabled(g_settings.general.start_with_windows);
            // Restart the recorder so replay length/fps/bitrate changes apply live.
            stop_recorder();
            start_recorder();
            notify_user(L"Settings saved", L"ClipLite");
            break;
        case WM_TIMER: {
            const auto app = g_detector.detect_foreground();
            const auto sel = g_selector.update(app, GetTickCount64());
            const std::string new_game = sel.active ? sel.game_name : std::string();
            const std::string new_exe = sel.active ? sel.exe_name : std::string();
            if (new_game != g_current_game) {
                if (new_game.empty()) CL_INFO("App", "no game in foreground");
                else CL_INFO("App", "detected game: " + new_game);
            }
            g_current_game = new_game;
            g_current_exe = new_exe;
            if (!g_current_exe.empty() && g_current_exe != g_last_reported_exe) {
                g_last_reported_exe = g_current_exe;
                if (g_library) g_library->notify_detected_game(g_current_exe);
            }
            {
                std::lock_guard<std::mutex> lock(g_recorder_mutex);
                if (g_recorder) g_recorder->set_game_name(g_current_game);
            }

            // Idle RAM saving + source selection: capture only when the
            // foreground game is allow-listed (or desktop capture is on).
            bool game_allowed = false;
            if (!g_current_exe.empty()) {
                const auto it = g_settings.games.find(g_current_exe);
                game_allowed = it == g_settings.games.end() ? true : it->second.auto_capture;
            }
            const bool want_capture =
                (game_allowed && g_settings.general.auto_capture) ||
                g_settings.general.capture_desktop_idle;
            static bool last_want = want_capture;
            static uint64_t last_flip = GetTickCount64();
            if (want_capture != last_want) {
                last_want = want_capture;
                last_flip = GetTickCount64();
            }
            const uint64_t now = GetTickCount64();
            const bool running = g_recorder_thread.joinable();
            if (want_capture && !running && now - last_flip > 2000) {
                CL_INFO("App", "capture target present, starting recorder");
                start_recorder();
            } else if (!want_capture && running && now - last_flip > 5000) {
                CL_INFO("App", "idle with no capture target, sleeping recorder");
                stop_recorder();
            }

            // Legible state: hover tooltip always says what ClipLite is doing.
            {
                std::wstring tip;
                if (g_recorder_thread.joinable()) {
                    tip = L"ClipLite \u2014 Recording " +
                          (g_current_game.empty()
                               ? std::wstring(L"Desktop")
                               : cliplite::util::utf8_to_wide(g_current_game));
                } else {
                    tip = L"ClipLite \u2014 Idle (waiting for a game)";
                }
                static std::wstring last_tip;
                if (tip != last_tip) {
                    last_tip = tip;
                    g_tray.set_tip(tip);
                }
            }

            // One teaching balloon per new app, ever (this session): explains
            // the model (F8 saves the buffer) and where to opt out.
            if (want_capture && !new_exe.empty()) {
                static std::set<std::string> announced;
                const bool known =
                    g_settings.games.find(new_exe) != g_settings.games.end();
                if (!known && !announced.count(new_exe)) {
                    announced.insert(new_exe);
                    notify_user(
                        L"Recording " + cliplite::util::utf8_to_wide(new_game),
                        L"Press F8 anytime to save the last 60 seconds. "
                        L"Settings \u2192 Games can exclude this app.");
                }
            }

            if (g_library && g_library->visible()) {
                const std::string hk =
                    cliplite::util::wide_to_utf8(
                        [&] {
                            wchar_t buf[32]{};
                            MultiByteToWideChar(CP_UTF8, 0, g_settings.hotkeys.save_clip.c_str(),
                                                -1, buf, 32);
                            return std::wstring(buf);
                        }());
                g_library->push_status(g_recorder_thread.joinable(), g_current_game, hk);
            }
            break;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            stop_recorder();
            g_tray.remove();
            UnregisterHotKey(hwnd, HOTKEY_SAVE_CLIP);
            PostQuitMessage(0);
            break;
        default:
            if (msg == g_wmTaskbarCreated) {
                // Explorer restarted: recreate the tray icon.
                g_tray.add(hwnd, WM_APP_TRAY, L"ClipLite");
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

bool register_window_class() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kWindowClassName;
    return RegisterClassExW(&wc) != 0;
}

// Runs full init (COM, hidden window, hotkey, tray), reports each step to the
// log, then tears down. Returns 0 when every step succeeds. Used for CI/smoke
// verification without leaving a stray tray icon behind.
int run_selftest() {
    cliplite::log::init("cliplite_selftest.log");
    CL_INFO("Selftest", "starting");
    bool ok = true;

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) {
        CL_INFO("Selftest", "COM init ok");
    } else {
        CL_ERROR("Selftest", "COM init failed");
        ok = false;
    }

    if (register_window_class()) {
        CL_INFO("Selftest", "window class ok");
    } else {
        CL_ERROR("Selftest", "window class registration failed");
        ok = false;
    }

    g_hwnd = CreateWindowExW(0, kWindowClassName, L"ClipLite", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0,
                             nullptr, nullptr, g_hInst, nullptr);
    if (g_hwnd) {
        CL_INFO("Selftest", "hidden window created");
    } else {
        CL_ERROR("Selftest", "window creation failed");
        ok = false;
    }

    if (g_hwnd && RegisterHotKey(g_hwnd, HOTKEY_SAVE_CLIP, 0, VK_F8)) {
        CL_INFO("Selftest", "global hotkey F8 registered");
    } else {
        CL_ERROR("Selftest", "global hotkey registration failed");
        ok = false;
    }

    if (g_hwnd && g_tray.add(g_hwnd, WM_APP_TRAY, L"ClipLite")) {
        CL_INFO("Selftest", "tray icon added");
    } else {
        CL_ERROR("Selftest", "tray icon add failed");
        ok = false;
    }

    if (g_hwnd && g_tray.notify(L"ClipLite", L"Selftest OK")) {
        CL_INFO("Selftest", "balloon notification ok");
    }

    g_tray.remove();
    if (g_hwnd) {
        UnregisterHotKey(g_hwnd, HOTKEY_SAVE_CLIP);
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    if (SUCCEEDED(hr)) CoUninitialize();

    CL_INFO("Selftest", ok ? "PASS" : "FAIL");
    cliplite::log::shutdown();
    return ok ? 0 : 1;
}

// Creates the library window, refreshes the clip list from the real clip dir,
// reports the count, and tears down. Verifies the UI construction path.
int run_library_selftest() {
    cliplite::log::init("cliplite_library_selftest.log");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    auto lib = std::make_unique<cliplite::ui::LibraryWindow>();
    const bool ok = lib->create(g_hInst, default_clip_dir());
    if (ok) {
        lib->refresh_clips();
        CL_INFO("Selftest", "library window created, clips=" + std::to_string(lib->clip_count()));
    } else {
        CL_ERROR("Selftest", "library window creation failed");
    }
    lib.reset();

    CoUninitialize();
    cliplite::log::shutdown();
    return ok ? 0 : 1;
}

int run_normal(bool open_library_on_start) {
    // Single instance: two recorders fighting over desktop duplication cause
    // DXGI access-denied storms and doubled encoder churn.
    const HANDLE single_instance =
        CreateMutexW(nullptr, TRUE, L"Local\\ClipLiteSingleInstance");
    if (!single_instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single_instance) CloseHandle(single_instance);
        MessageBoxW(nullptr, L"ClipLite is already running (check the system tray).",
                    L"ClipLite", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    cliplite::log::init(w2u8(env_or(L"LOCALAPPDATA", L"C:\\Temp") + L"\\ClipLite\\logs\\cliplite.log"));

    g_settings = cliplite::Settings::defaults();
    if (g_settings.load(settings_path())) {
        CL_INFO("App", "settings loaded");
    }
    if (!g_settings.hotkeys.save_clip.empty()) {
        if (auto hk = cliplite::parse_hotkey(g_settings.hotkeys.save_clip)) {
            g_hotkey_mods = hk->modifiers;
            g_hotkey_vk = hk->vk;
        } else {
            CL_WARN("App", "could not parse save-clip hotkey; defaulting to F8");
        }
    } else {
        CL_INFO("App", "save-clip hotkey unbound in settings");
    }
    cliplite::app::set_startup_enabled(g_settings.general.start_with_windows);

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        CL_ERROR("App", "COM init failed");
        return 1;
    }

    if (!register_window_class()) {
        CL_ERROR("App", "window class registration failed");
        CoUninitialize();
        return 1;
    }

    g_hwnd = CreateWindowExW(0, kWindowClassName, L"ClipLite", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0,
                             nullptr, nullptr, g_hInst, nullptr);
    if (!g_hwnd) {
        CL_ERROR("App", "window creation failed");
        CoUninitialize();
        return 1;
    }

    if (!RegisterHotKey(g_hwnd, HOTKEY_SAVE_CLIP, g_hotkey_mods, g_hotkey_vk)) {
        CL_WARN("App", "failed to register save-clip hotkey (already in use?)");
    }
    g_tray.add(g_hwnd, WM_APP_TRAY, L"ClipLite");

    SetTimer(g_hwnd, 1, 1000, nullptr);
    // Recorder starts on demand (allow-listed game detected or desktop capture on).
    if ((g_settings.general.auto_capture && !g_settings.games.empty()) ||
        g_settings.general.capture_desktop_idle || g_current_exe.empty() == false) {
        start_recorder();
    } else {
        CL_INFO("App", "starting idle: recorder asleep until a game is detected");
    }
    if (open_library_on_start) open_library();
    CL_INFO("App", "ClipLite running in tray");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    CL_INFO("App", "ClipLite exiting");
    cliplite::log::shutdown();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    g_hInst = hInstance;
    bool open_library_on_start = false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--selftest") == 0) {
                LocalFree(argv);
                return run_selftest();
            }
            if (_wcsicmp(argv[i], L"--library-selftest") == 0) {
                LocalFree(argv);
                return run_library_selftest();
            }
            if (_wcsicmp(argv[i], L"--open") == 0 || _wcsicmp(argv[i], L"--library") == 0) {
                open_library_on_start = true;
                continue;
            }
            if (_wcsicmp(argv[i], L"--version") == 0 || _wcsicmp(argv[i], L"-v") == 0) {
                AttachConsole(ATTACH_PARENT_PROCESS);
                console_write(L"ClipLite 0.1.0\n");
                LocalFree(argv);
                return 0;
            }
        }
        LocalFree(argv);
    }

    return run_normal(open_library_on_start);
}
