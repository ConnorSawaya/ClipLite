#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace cliplite {
struct Settings;
}

namespace cliplite::ui {

struct WebClip {
    std::wstring id;      // filename
    std::wstring path;
    std::string game;
    std::string date;
    std::string dur;
    uint64_t size = 0;
    int64_t duration_ms = 0;
};

// WebView2-hosted library window. The visible UI is static HTML/CSS/JS under
// assets/web, talking to this class via postMessage. The recorder, tray and
// hotkeys stay fully native. Same lifecycle contract as before: close hides
// the window, the background recorder keeps running.
class LibraryWindow {
public:
    LibraryWindow() = default;
    ~LibraryWindow();
    LibraryWindow(const LibraryWindow&) = delete;
    LibraryWindow& operator=(const LibraryWindow&) = delete;

    bool create(HINSTANCE hInstance, const std::wstring& clip_dir);
    void show();
    void hide();
    bool visible() const;
    HWND hwnd() const { return hwnd_; }
    void refresh_clips();
    std::size_t clip_count() const { return clips_.size(); }

    // Native -> UI plumbing.
    void set_clip_request(std::function<void()> fn);
    void set_settings_request(std::function<void()> fn);
    void push_status(bool recording, const std::string& game, const std::string& hotkey);

    // Live settings backend for the in-UI settings panel.
    void attach_settings(cliplite::Settings* settings, const std::string& path,
                         HWND notify_hwnd, UINT notify_msg);
    void open_settings_ui();

    // Feed the currently-detected game exe so the Sources list can offer it.
    void notify_detected_game(const std::string& exe);

    // Implementation slot used by the WebView2 completion handlers.
    void* impl_slot() const { return env_; }

private:
    void send_clips();
    void handle_message(const std::wstring& json);
    void open_player(const std::wstring& id);
    const WebClip* find_clip(const std::wstring& id) const;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    std::wstring clip_dir_;
    std::vector<WebClip> clips_;

    // WebView2 COM pointers (declared as IUnknown* here; real types used in cpp).
    void* env_ = nullptr;        // ICoreWebView2Environment*
    void* controller_ = nullptr; // ICoreWebView2Controller*
    void* webview_ = nullptr;    // ICoreWebView2*
    void* msg_token_ = nullptr;  // EventRegistrationToken storage
    bool ui_ready_ = false;
    bool status_dirty_ = false;
    bool last_recording_ = false;
    std::string last_game_;
    std::string last_hotkey_;

    std::function<void()> clip_request_;
    std::function<void()> settings_request_;
    cliplite::Settings* settings_ = nullptr;
    std::string settings_path_;
    HWND settings_notify_ = nullptr;
    UINT settings_notify_msg_ = 0;
};

}  // namespace cliplite::ui
