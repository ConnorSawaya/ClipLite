#include "cliplite/ui/library_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"

#include "cliplite/audio/wasapi_mic.h"

#include "cliplite/library/clip_library.h"
#include "cliplite/library/thumbnails.h"
#include "cliplite/config.h"
#include "cliplite/detection/window_info.h"
#include "cliplite/util/win_utf8.h"
#include "cliplite/log.h"
#include <wrl/client.h>

namespace cliplite::ui {

namespace {

constexpr UINT IDM_PLAY_BASE = 0;

const wchar_t* kWindowClass = L"ClipLiteLibraryWebWindow";
const COLORREF kBg = RGB(20, 21, 24);
const COLORREF kFrameBorder = RGB(70, 73, 81);
const COLORREF kText = RGB(243, 244, 246);
const wchar_t* kAppHost = L"https://app.cliplite.local";
const wchar_t* kMediaHost = L"https://media.cliplite.local";
const wchar_t* kThumbsHost = L"https://thumbs.cliplite.local";

constexpr UINT IDD_RENAME = 300;
constexpr UINT IDC_RENAME_EDIT = 301;

void style_window_frame(HWND hwnd) {
    const BOOL dark = TRUE;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    // These attributes are optional; unsupported Windows versions keep system chrome.
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &kBg, sizeof(kBg));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &kText, sizeof(kText));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &kFrameBorder, sizeof(kFrameBorder));
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
}

std::wstring format_duration(int64_t ms) {
    wchar_t buf[32];
    swprintf_s(buf, L"%02lld:%02lld", static_cast<long long>(ms / 60000),
               static_cast<long long>((ms / 1000) % 60));
    return buf;
}

std::string date_label_from_filename(const std::wstring& filename) {
    const auto us = filename.find(L'_');
    if (us == std::wstring::npos) return cliplite::util::wide_to_utf8(filename);
    const std::wstring stamp = filename.substr(us + 1);
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (swscanf_s(stamp.c_str(), L"%d-%d-%d_%d-%d-%d", &y, &mo, &d, &hh, &mm, &ss) != 6) {
        return cliplite::util::wide_to_utf8(stamp);
    }
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %d %02d:%02d", months[(mo - 1) % 12], d, hh, mm);
    return buf;
}

std::wstring json_str(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        switch (c) {
            case L'"': o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            default:
                if (c < 0x20) o += L'?';
                else o += c;
        }
    }
    return o + L"\"";
}

bool extract_field(const std::wstring& json, const wchar_t* key, std::wstring& out) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":\"";
    const auto pos = json.find(needle);
    if (pos == std::wstring::npos) return false;
    size_t i = pos + needle.size();
    std::wstring val;
    auto hex4 = [&](size_t at, unsigned& v) {
        v = 0;
        if (at + 4 > json.size()) return false;
        for (size_t k = 0; k < 4; ++k) {
            const wchar_t c = json[at + k];
            v <<= 4;
            if (c >= L'0' && c <= L'9') v |= static_cast<unsigned>(c - L'0');
            else if (c >= L'a' && c <= L'f') v |= static_cast<unsigned>(c - L'a' + 10);
            else if (c >= L'A' && c <= L'F') v |= static_cast<unsigned>(c - L'A' + 10);
            else return false;
        }
        return true;
    };
    while (i < json.size() && json[i] != L'"') {
        if (json[i] == L'\\' && i + 1 < json.size()) {
            ++i;
            switch (json[i]) {
                case L'"': val += L'"'; break;
                case L'\\': val += L'\\'; break;
                case L'/': val += L'/'; break;
                case L'b': val += L'\b'; break;
                case L'f': val += L'\f'; break;
                case L'n': val += L'\n'; break;
                case L'r': val += L'\r'; break;
                case L't': val += L'\t'; break;
                case L'u': {
                    // Full JSON string escapes (needed for control chars the
                    // page sends, e.g. unit/record separators in text lists).
                    // BMP only; lone surrogates are dropped, not mis-decoded.
                    unsigned v = 0;
                    if (hex4(i + 1, v) && v != 0) {
                        if (v >= 0xD800 && v <= 0xDBFF) {
                            unsigned lo = 0;
                            if (i + 6 < json.size() && json[i + 1] == L'\\' &&
                                (json[i + 2] == L'u' || json[i + 2] == L'U') &&
                                hex4(i + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                                // wchar_t is UTF-16: emit the pair.
                                const unsigned cp =
                                    ((v - 0xD800) << 10) + (lo - 0xDC00) + 0x10000;
                                val += static_cast<wchar_t>(0xD800 + (cp >> 10));
                                val += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
                                i += 6;
                            }
                            // else: drop the lone high surrogate.
                        } else if (v >= 0xDC00 && v <= 0xDFFF) {
                            // Lone low surrogate: drop.
                        } else {
                            val += static_cast<wchar_t>(v);
                        }
                        i += 4;
                    }
                    // else: malformed escape, drop the 'u'.
                    break;
                }
                default: val += json[i];
            }
        } else {
            val += json[i];
        }
        ++i;
    }
    out = val;
    return true;
}

int extract_int(const std::wstring& json, const wchar_t* key, int fallback) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    const auto pos = json.find(needle);
    if (pos == std::wstring::npos) return fallback;
    return static_cast<int>(wcstol(json.c_str() + pos + needle.size(), nullptr, 10));
}

// Raw numeric token as a string ("12", "1.5"); empty when absent.
std::wstring extract_num(const std::wstring& json, const wchar_t* key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    const auto pos = json.find(needle);
    if (pos == std::wstring::npos) return L"";
    size_t i = pos + needle.size();
    std::wstring val;
    while (i < json.size() &&
           (iswdigit(json[i]) || json[i] == L'.' || json[i] == L'-' || json[i] == L'+')) {
        val += json[i++];
    }
    return val;
}

// Returns `fallback` when the key is absent; otherwise the parsed boolean.
bool extract_bool(const std::wstring& json, const wchar_t* key, bool fallback) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    const auto pos = json.find(needle);
    if (pos == std::wstring::npos) return fallback;
    const size_t v = pos + needle.size();
    if (json.compare(v, 4, L"true") == 0) return true;
    if (json.compare(v, 5, L"false") == 0) return false;
    return fallback;
}

std::wstring url_escape_segment(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
            case L'%': o += L"%25"; break;
            case L'#': o += L"%23"; break;
            case L'?': o += L"%3F"; break;
            case L' ': o += L"%20"; break;
            default: o += c;
        }
    }
    return o;
}

// Enumerate DXGI outputs for the source dropdown: "Display N · WxH".
std::vector<std::pair<int, std::string>> enumerate_displays() {
    std::vector<std::pair<int, std::string>> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory))))
        return out;
    int idx = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc))) {
                MONITORINFO mi{sizeof(mi)};
                if (GetMonitorInfoW(desc.Monitor, &mi)) {
                    char label[96];
                    snprintf(label, sizeof(label), "Display %d \xC2\xB7 %ldx%ld%s", idx + 1,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             idx == 0 ? " (primary)" : "");
                    out.emplace_back(idx, label);
                }
                ++idx;
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}

INT_PTR CALLBACK rename_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        style_window_frame(dlg);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        auto* name = reinterpret_cast<std::wstring*>(lp);
        SetDlgItemTextW(dlg, IDC_RENAME_EDIT, name->c_str());
        SendDlgItemMessageW(dlg, IDC_RENAME_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_RENAME_EDIT));
        return FALSE;
    }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[512]{};
            GetDlgItemTextW(dlg, IDC_RENAME_EDIT, buf, 512);
            auto* name = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dlg, DWLP_USER));
            if (name) *name = buf;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}


}  // namespace

// ---------------------------------------------------------------------------
// WebView2 completion/message handlers (COM boilerplate).
// ---------------------------------------------------------------------------

namespace {

template <typename Derived, typename Interface>
struct ComHandlerBase : Interface {
    ULONG refs_ = 0;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface)) {
            *ppv = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG c = --refs_;
        if (c == 0) delete static_cast<Derived*>(this);
        return c;
    }
};

struct EnvCreatedHandler
    : ComHandlerBase<EnvCreatedHandler, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    LibraryWindow* api = nullptr;
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code,
                                     ICoreWebView2Environment* env) override;
};

struct ControllerCreatedHandler
    : ComHandlerBase<ControllerCreatedHandler,
                     ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
    LibraryWindow* api = nullptr;
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code,
                                     ICoreWebView2Controller* controller) override;
};

struct ContentLoadingHandler
    : ComHandlerBase<ContentLoadingHandler, ICoreWebView2ContentLoadingEventHandler> {
    LibraryWindow* api = nullptr;
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2ContentLoadingEventArgs* args) override;
};

struct MessageReceivedHandler
    : ComHandlerBase<MessageReceivedHandler, ICoreWebView2WebMessageReceivedEventHandler> {
    LibraryWindow* api = nullptr;
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override;
};

struct ScriptExecHandler
    : ComHandlerBase<ScriptExecHandler, ICoreWebView2ExecuteScriptCompletedHandler> {
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT, LPCWSTR result_json) override {
        if (result_json)
            CL_INFO("Library", "exec: " + cliplite::util::wide_to_utf8(result_json));
        else
            CL_INFO("Library", "exec: <null>");
        return S_OK;
    }
};

}  // namespace

class alignas(8) LibraryWindowImpl {
public:
    LibraryWindow* api = nullptr;
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;
    std::wstring clip_dir;
    std::vector<WebClip> clips;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> web;
    EventRegistrationToken msg_token{};
    bool msg_hooked = false;
    bool ui_ready = false;
    bool have_clips = false;
    bool recording = false;
    std::wstring game;
    std::wstring hotkey = L"F8";
    std::function<void()> clip_request;
    std::function<void()> settings_request;
    cliplite::Settings* settings = nullptr;
    std::string settings_path;
    HWND settings_notify = nullptr;
    UINT settings_notify_msg = 0;
    std::string detected_exe;

    bool env_pending = false;

    // Mic level preview (popover).
    std::unique_ptr<audio::MicPreview> mic_preview;
    std::mutex lvl_mu;
    std::map<std::wstring, int> latest_levels;

    bool ensure_web_started();
    void shutdown_web();
    void scan();
    void send_clips();
    void send_sources();
    void send_status();
    void poll_mic_levels();
    void on_web_message(ICoreWebView2WebMessageReceivedEventArgs* args);
    void run_command(const std::wstring& cmd, const std::wstring& id,
                     const std::wstring& json);
    const WebClip* find(const std::wstring& id) const;
};

void push_json(LibraryWindowImpl* impl, const std::wstring& json);

void LibraryWindowImpl::scan() {
    clips.clear();
    std::error_code ec;
    std::vector<std::pair<std::wstring, int64_t>> found;
    for (const auto& entry : std::filesystem::directory_iterator(clip_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != L".mp4") continue;
        found.emplace_back(entry.path().filename().wstring(),
                           static_cast<int64_t>(entry.file_size(ec)));
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [name, size] : found) {
        WebClip c;
        c.id = name;
        c.path = clip_dir + L"\\" + name;
        const auto stem_us = name.find(L'_');
        c.game = cliplite::util::wide_to_utf8(
            stem_us == std::wstring::npos ? name : name.substr(0, stem_us));
        c.date = date_label_from_filename(name);
        const int64_t dur_ms = cliplite::library::ClipLibrary::probe_duration_ms(c.path);
        // Unopenable files (0-byte/truncated leftovers from failed renders)
        // would show as broken cards with dead thumbnails: skip them so the
        // library only lists playable clips.
        if (dur_ms <= 0) {
            CL_WARN("Library", "skipping unplayable file: " +
                                   cliplite::util::wide_to_utf8(name));
            continue;
        }
        c.dur = cliplite::util::wide_to_utf8(format_duration(dur_ms));
        c.duration_ms = dur_ms;
        c.size = static_cast<uint64_t>(size);
        clips.push_back(std::move(c));
    }
}

void LibraryWindowImpl::send_clips() {
    if (!web || !ui_ready) {
        have_clips = true;  // flush once the page finishes loading
        return;
    }
    CL_INFO("Library", "sending " + std::to_string(clips.size()) + " clips to UI");
    std::wstring js = L"{\"type\":\"clips\",\"clips\":[";
    bool first = true;
    for (const auto& c : clips) {
        if (!first) js += L",";
        first = false;
        js += L"{\"id\":" + json_str(c.id);
        js += L",\"game\":" + json_str(cliplite::util::utf8_to_wide(c.game));
        js += L",\"date\":" + json_str(cliplite::util::utf8_to_wide(c.date));
        js += L",\"dur\":" + json_str(cliplite::util::utf8_to_wide(c.dur));
        js += L",\"size\":" + std::to_wstring(c.size);
        js += L",\"dur_ms\":" + std::to_wstring(c.duration_ms);
        js += L",\"thumb\":\"\"";
        js += L"}";
    }
    js += L"]}";
    push_json(this, js);

    // Thumbnails one message each: cached BMPs are served by the WebView2
    // virtual host, so each message is just a short URL. Missing entries are
    // generated once (which writes the cache) and served the same way.
    for (const auto& c : clips) {
        std::wstring file = cliplite::library::cached_thumbnail_file(c.path, 240, 135);
        if (file.empty()) {
            HBITMAP tb = cliplite::library::generate_thumbnail(c.path, 240, 135);
            if (tb) DeleteObject(tb);
            file = cliplite::library::cached_thumbnail_file(c.path, 240, 135);
        }
        if (file.empty()) {
            CL_WARN("Library", "thumbnail failed: " + cliplite::util::wide_to_utf8(c.id));
            continue;
        }
        const auto slash = file.find_last_of(L"\\/");
        const std::wstring name = (slash == std::wstring::npos) ? file : file.substr(slash + 1);
        std::wstring tj = L"{\"type\":\"thumb\",\"id\":" + json_str(c.id);
        tj += L",\"thumb\":" + json_str(std::wstring(kThumbsHost) + L"/" + name) + L"}";
        push_json(this, tj);
    }
    have_clips = false;
}

void LibraryWindowImpl::send_status() {
    if (!web || !ui_ready) return;
    std::wstring js = L"{\"type\":\"status\",\"recording\":";
    js += recording ? L"true" : L"false";
    js += L",\"game\":" + json_str(game);
    js += L",\"hotkey\":" + json_str(hotkey);
    if (settings) {
        js += settings->audio.desktop_enabled ? L",\"desktop\":true" : L",\"desktop\":false";
        js += settings->audio.mic_enabled ? L",\"mic\":true" : L",\"mic\":false";
        js += L",\"display\":" +
              std::to_wstring(std::max(0, settings->recording.display_index));
    }
    js += L"}";
    push_json(this, js);

    // One-shot DOM ground truth after the first status tick.
    static bool probed = false;
    if (!probed && web) {
        probed = true;
        web->ExecuteScript(
            L"JSON.stringify({cards:document.querySelectorAll('.card').length,"
            L"clipsLen:(typeof clips!=='undefined')?clips.length:-1,"
            L"gridHtmlLen:document.getElementById('grid').innerHTML.length,"
            L"emptyShown:document.getElementById('empty').classList.contains('show')})",
            new ScriptExecHandler());
    }
}

void LibraryWindowImpl::send_sources() {
    if (!web || !ui_ready) return;
    // One roundtrip for the whole Source menu: mode + displays + windows.
    std::wstring js = L"{\"type\":\"sources\"";
    js += L",\"mode\":" +
          json_str(settings ? cliplite::util::utf8_to_wide(settings->recording.source_mode)
                            : L"display");
    js += L",\"window_exe\":";
    js += json_str(settings ? cliplite::util::utf8_to_wide(
                                  settings->recording.source_window_exe)
                            : L"");
    js += L",\"window_title\":";
    js += json_str(settings ? cliplite::util::utf8_to_wide(
                                  settings->recording.source_window_title)
                            : L"");
    js += L",\"display\":";
    js += std::to_wstring(settings ? std::max(0, settings->recording.display_index) : 0);
    js += L",\"displays\":[";
    bool first = true;
    for (auto& [i, label] : enumerate_displays()) {
        if (!first) js += L",";
        first = false;
        js += L"{\"i\":" + std::to_wstring(i) +
              L",\"label\":" + json_str(cliplite::util::utf8_to_wide(label)) + L"}";
    }
    js += L"],\"windows\":[";
    first = true;
    for (auto& w : cliplite::detection::enum_visible_windows()) {
        if (!first) js += L",";
        first = false;
        js += L"{\"exe\":" + json_str(cliplite::util::utf8_to_wide(w.exe_name)) +
              L",\"title\":" + json_str(w.title) + L"}";
    }
    js += L"]}";
    push_json(this, js);
}

const WebClip* LibraryWindowImpl::find(const std::wstring& id) const {
    for (const auto& c : clips)
        if (c.id == id) return &c;
    return nullptr;
}

bool LibraryWindowImpl::ensure_web_started() {
    if (web || env_pending) return true;
    const std::wstring udf = [] {
        wchar_t buf[MAX_PATH]{};
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        return std::wstring(buf) + L"\\ClipLite\\WebView2";
    }();
    EnvCreatedHandler* h = new EnvCreatedHandler();
    h->api = api;
    env_pending = true;
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr, h);
    CL_INFO("Library", "CreateCoreWebView2EnvironmentWithOptions hr=" +
                           [&] {
                               char buf[16];
                               snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
                               return std::string(buf);
                           }());
    if (FAILED(hr)) {
        env_pending = false;
        CL_ERROR("Library", "CreateCoreWebView2EnvironmentWithOptions failed");
        return false;
    }
    return true;
}

// Releases the whole WebView2 stack so a hidden window costs ~nothing.
// State flags make the next show() rebuild + re-handshake cleanly.
void LibraryWindowImpl::shutdown_web() {
    if (msg_hooked && web) web->remove_WebMessageReceived(msg_token);
    msg_hooked = false;
    ui_ready = false;
    have_clips = true;
    web.Reset();
    controller.Reset();
    env.Reset();
}

void LibraryWindowImpl::poll_mic_levels() {
    if (!web || !ui_ready) return;
    std::map<std::wstring, int> levels;
    {
        std::lock_guard<std::mutex> lk(lvl_mu);
        levels.swap(latest_levels);
    }
    if (levels.empty()) return;
    std::wstring js = L"{\"type\":\"miclevels\",\"levels\":[";
    bool first = true;
    for (const auto& [id, pct] : levels) {
        if (!first) js += L",";
        first = false;
        js += L"{\"i\":" + json_str(id) + L",\"p\":" + std::to_wstring(pct) + L"}";
    }
    js += L"]}";
    push_json(this, js);
}

// Native -> page delivery. ExecuteScript is proven reliable here; the JSON
// literal itself is a valid JS expression, so no quoting games are needed.
void push_json(LibraryWindowImpl* impl, const std::wstring& json) {
    if (!impl || !impl->web) return;
    impl->web->ExecuteScript((std::wstring(L"window.__onNative&&__onNative(") + json + L")")
                                 .c_str(),
                             nullptr);
}

void LibraryWindowImpl::on_web_message(ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return;
    const std::wstring json(raw);
    CoTaskMemFree(raw);

    std::wstring cmd, id;
    if (!extract_field(json, L"cmd", cmd)) return;
    extract_field(json, L"id", id);

    if (cmd == L"jserror") {
        std::wstring msg;
        extract_field(json, L"msg", msg);
        CL_ERROR("Library", "UI JS error: " + cliplite::util::wide_to_utf8(msg));
        return;
    }
    if (cmd == L"ready") {
        // Page scripts are live and listening; safe to push everything now.
        ui_ready = true;
        if (have_clips) {
            scan();  // rescan so clips saved before load aren't stale
        }
        send_clips();
        send_status();
        return;
    }
    if (cmd == L"list_displays") {
        std::wstring js = L"{\"type\":\"displays\",\"active\":" +
                          std::to_wstring(settings
                                              ? std::max(0, settings->recording.display_index)
                                              : 0);
        js += L",\"list\":[";
        auto list = enumerate_displays();
        bool first = true;
        for (auto& [i, label] : list) {
            if (!first) js += L",";
            first = false;
            js += L"{\"i\":" + std::to_wstring(i) +
                  L",\"label\":" + json_str(cliplite::util::utf8_to_wide(label)) + L"}";
        }
        js += L"]}";
        push_json(this, js);
        return;
    }
    if (cmd == L"set_display") {
        const int i = extract_int(json, L"i", 0);
        if (settings && i >= 0) {
            settings->recording.display_index = i;
            settings->save(settings_path);
            if (settings_notify)
                PostMessageW(settings_notify, settings_notify_msg, 0, 0);
            push_json(this, L"{\"type\":\"toast\",\"text\":\"Display set (applies on next capture start)\"}");
        }
        return;
    }
    if (cmd == L"list_sources") {
        send_sources();
        return;
    }
    if (cmd == L"set_source") {
        if (!settings) return;
        std::wstring mode;
        extract_field(json, L"mode", mode);
        if (mode != L"window") mode = L"display";
        settings->recording.source_mode = cliplite::util::wide_to_utf8(mode);
        if (mode == L"window") {
            std::wstring exe, title;
            extract_field(json, L"exe", exe);
            extract_field(json, L"title", title);
            settings->recording.source_window_exe = cliplite::util::wide_to_utf8(exe);
            settings->recording.source_window_title = cliplite::util::wide_to_utf8(title);
        }
        const int i = extract_int(json, L"display", -1);
        if (i >= 0) settings->recording.display_index = i;
        settings->save(settings_path);
        if (settings_notify)
            PostMessageW(settings_notify, settings_notify_msg, 0, 0);
        if (mode == L"window")
            push_json(this,
                      L"{\"type\":\"toast\",\"text\":\"Window selected \u2014 isolated "
                      L"capture lands in the next update; recording the display for now\"}");
        else
            push_json(this, L"{\"type\":\"toast\",\"text\":\"Source set to entire display\"}");
        // Refresh the menu state so the label follows the choice.
        send_sources();
        return;
    }
    if (cmd == L"set_audio") {
        if (!settings) return;
        settings->audio.desktop_enabled = extract_bool(json, L"desktop", true);
        settings->audio.mic_enabled = extract_bool(json, L"mic", false);
        settings->save(settings_path);
        if (settings_notify)
            PostMessageW(settings_notify, settings_notify_msg, 0, 0);
        return;
    }
    if (cmd == L"list_mics") {
        auto devs = audio::enumerate_mics();
        std::wstring active;
        if (settings) {
            active = cliplite::util::utf8_to_wide(settings->audio.mic_device);
            if (active.empty()) active = audio::default_mic_id();
        }
        std::wstring js = L"{\"type\":\"mics\",\"active\":" + json_str(active) + L",\"list\":[";
        bool first = true;
        for (auto& d : devs) {
            if (!first) js += L",";
            first = false;
            js += L"{\"id\":" + json_str(d.id) +
                  L",\"name\":" + json_str(cliplite::util::utf8_to_wide(d.name)) + L"}";
        }
        js += L"]}";
        push_json(this, js);
        return;
    }
    if (cmd == L"set_mic") {
        std::wstring id;
        extract_field(json, L"id", id);
        if (settings) {
            settings->audio.mic_device = cliplite::util::wide_to_utf8(id);
            settings->save(settings_path);
            if (settings->audio.mic_enabled && settings_notify)
                PostMessageW(settings_notify, settings_notify_msg, 0, 0);
        }
        return;
    }
    if (cmd == L"mic_preview_on") {
        std::vector<std::wstring> ids;
        for (auto& d : audio::enumerate_mics()) ids.push_back(d.id);
        lvl_mu.lock();
        latest_levels.clear();
        lvl_mu.unlock();
        mic_preview = audio::MicPreview::start(ids, [this](const std::wstring& id, int pct) {
            std::lock_guard<std::mutex> lk(lvl_mu);
            latest_levels[id] = pct;
        });
        return;
    }
    if (cmd == L"mic_preview_off") {
        mic_preview.reset();
        return;
    }

    run_command(cmd, id, json);
}

// Removes capture metadata, legacy edit projects, and audio stems beside a clip.
void delete_clip_sidecars(const std::wstring& clip_path);

void LibraryWindowImpl::run_command(const std::wstring& cmd, const std::wstring& id,
                                     const std::wstring& json) {
    const WebClip* clip = id.empty() ? nullptr : find(id);

    if (cmd == L"refresh") {
        scan();
        send_clips();
        return;
    }
    if (cmd == L"clip_now") {
        if (clip_request) clip_request();
        return;
    }
    if (cmd == L"settings") {
        if (settings_request) settings_request();
        return;
    }
    if (cmd == L"get_settings") {
        if (!settings) return;
        cliplite::Settings* s = settings;
        std::wstring js = L"{\"type\":\"settings\"";
        js += L",\"replay\":" + std::to_wstring(s->recording.replay_duration_sec);
        js += L",\"fps\":" + std::to_wstring(s->recording.fps);
        js += L",\"bitrate\":" + std::to_wstring(s->recording.bitrate_mbps);
        js += L",\"quality\":" + json_str(cliplite::util::utf8_to_wide(s->general.quality_preset));
        js += s->audio.desktop_enabled ? L",\"desktop\":true" : L",\"desktop\":false";
        js += s->audio.mic_enabled ? L",\"mic\":true" : L",\"mic\":false";
        js += L",\"hotkey\":" + json_str(cliplite::util::utf8_to_wide(s->hotkeys.save_clip));
        js += s->general.start_with_windows ? L",\"startup\":true" : L",\"startup\":false";
        js += s->general.notifications ? L",\"notifications\":true" : L",\"notifications\":false";
        js += s->general.capture_desktop_idle ? L",\"idlecap\":true" : L",\"idlecap\":false";
        js += s->general.auto_capture ? L",\"gamesmaster\":true" : L",\"gamesmaster\":false";
        // Known games + the currently-detected one (so it can be configured
        // before its first save).
        js += L",\"games\":[";
        bool first_game = true;
        auto push_game = [&](const std::string& exe, bool capture) {
            if (!first_game) js += L",";
            first_game = false;
            js += L"{\"n\":" + json_str(cliplite::util::utf8_to_wide(exe)) +
                  L",\"c\":" + (capture ? L"true" : L"false") + L"}";
        };
        for (const auto& [exe, gs] : s->games) push_game(exe, gs.auto_capture);
        if (!detected_exe.empty() && s->games.find(detected_exe) == s->games.end())
            push_game(detected_exe, true);
        js += L"]}";
        push_json(this, js);
        return;
    }
    if (cmd == L"save_settings") {
        if (!settings) return;
        cliplite::Settings* s = settings;
        int replay = extract_int(json, L"replay", 60);
        int fps = extract_int(json, L"fps", 60);
        int bitrate = extract_int(json, L"bitrate", 15);
        if (replay < 5) replay = 5;
        if (replay > 7200) replay = 7200;  // long-recording window (disk-backed)
        if (fps < 1) fps = 1;
        if (fps > 240) fps = 240;
        if (bitrate < 1) bitrate = 1;
        if (bitrate > 200) bitrate = 200;
        s->recording.replay_duration_sec = replay;
        s->recording.fps = fps;
        s->recording.bitrate_mbps = bitrate;

        std::wstring quality, hotkey;
        if (extract_field(json, L"quality", quality)) {
            const std::string q = cliplite::util::wide_to_utf8(quality);
            if (q == "low" || q == "balanced" || q == "high" || q == "custom")
                s->general.quality_preset = q;
        }
        s->audio.desktop_enabled = extract_bool(json, L"desktop", true);
        s->audio.mic_enabled = extract_bool(json, L"mic", false);
        if (extract_field(json, L"hotkey", hotkey)) {
            // Empty means unbound (the page sends "" for "(none)").
            s->hotkeys.save_clip = cliplite::util::wide_to_utf8(hotkey);
        }
        s->general.start_with_windows = extract_bool(json, L"startup", false);
        s->general.notifications = extract_bool(json, L"notifications", true);
        s->general.capture_desktop_idle = extract_bool(json, L"idlecap", false);
        s->general.auto_capture = extract_bool(json, L"gamesmaster", true);

        // Per-game allowlist: "exe=1,exe2=0". Flips auto_capture only, so other
        // per-game settings on existing entries are preserved.
        std::wstring games;
        if (extract_field(json, L"games", games)) {
            size_t p = 0;
            while (p < games.size()) {
                const size_t comma = games.find(L',', p);
                const std::wstring one =
                    comma == std::wstring::npos ? games.substr(p) : games.substr(p, comma - p);
                p = comma == std::wstring::npos ? games.size() : comma + 1;
                const size_t eq = one.rfind(L'=');
                if (eq == std::wstring::npos || eq == 0) continue;
                const std::string exe = cliplite::util::wide_to_utf8(one.substr(0, eq));
                if (exe.empty()) continue;
                const bool capture = one.compare(eq + 1, 1, L"1") == 0;
                auto it = s->games.find(exe);
                if (it == s->games.end()) {
                    cliplite::GameSettings gs;
                    gs.auto_capture = capture;
                    s->games[exe] = gs;
                } else {
                    it->second.auto_capture = capture;
                }
            }
        }

        if (s->save(settings_path)) {
            CL_INFO("Settings", "saved from UI to " + settings_path);
        } else {
            CL_ERROR("Settings", "UI save failed");
        }
        if (settings_notify)
            PostMessageW(settings_notify, settings_notify_msg, 0, 0);
        return;
    }
    if (cmd == L"play" && clip && web) {
        std::wstring js = L"{\"type\":\"play_url\",\"id\":" + json_str(id);
        js += L",\"url\":" + json_str(std::wstring(kMediaHost) + L"/" + url_escape_segment(id));
        js += L"}";
        push_json(this, js);
        return;
    }
    if (!clip) return;
    const std::filesystem::path p(clip->path);

    if (cmd == L"delete") {
        if (web) push_json(this, L"{\"type\":\"close_player\"}");
        Sleep(150);  // let the <video> release its file handle
        std::wstring from = clip->path;
        from.push_back(L'\0');
        from.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.hwnd = hwnd;
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOCONFIRMATION;
        SHFileOperationW(&op);
        delete_clip_sidecars(clip->path);
        scan();
        send_clips();
        return;
    }
    if (cmd == L"rename") {
        std::wstring newname = p.stem().wstring();
        if (DialogBoxParamW(hinst, MAKEINTRESOURCEW(IDD_RENAME), hwnd, rename_proc,
                            reinterpret_cast<LPARAM>(&newname)) == IDOK &&
            !newname.empty() && newname != p.stem().wstring()) {
            static const wchar_t* kBad = L"<>:\"/\\|?*";
            if (newname.find_first_of(kBad) != std::wstring::npos) {
                MessageBoxW(hwnd, L"That name contains characters not allowed in file names.",
                            L"Rename", MB_OK | MB_ICONWARNING);
                return;
            }
            if (web) push_json(this, L"{\"type\":\"close_player\"}");
            const std::wstring newpath = p.parent_path().wstring() + L"\\" + newname + L".mp4";
            if (MoveFileW(clip->path.c_str(), newpath.c_str())) {
                // Sidecars follow the rename (stems dir moves as a whole).
                std::error_code ec;
                std::filesystem::rename(clip->path + L".apps.json", newpath + L".apps.json",
                                        ec);
                std::filesystem::rename(clip->path + L".edit.json", newpath + L".edit.json",
                                        ec);
                if (clip->path.size() >= 4 && newpath.size() >= 4) {
                    std::filesystem::rename(
                        clip->path.substr(0, clip->path.size() - 4) + L".stems",
                        newpath.substr(0, newpath.size() - 4) + L".stems", ec);
                }
                scan();
                send_clips();
            } else {
                MessageBoxW(hwnd, L"Could not rename the clip.", L"Rename", MB_OK | MB_ICONERROR);
            }
        }
        return;
    }
    if (cmd == L"open") {
        const std::wstring sel = L"/select,\"" + clip->path + L"\"";
        ShellExecuteW(hwnd, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
        if (web) push_json(this, L"{\"type\":\"toast\",\"text\":\"Opened in Explorer\"}");
        return;
    }
    if (cmd == L"copy") {
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            const size_t len = (clip->path.size() + 2) * sizeof(wchar_t);
            if (HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + len)) {
                auto* df = static_cast<DROPFILES*>(GlobalLock(hg));
                df->pFiles = sizeof(DROPFILES);
                df->fWide = TRUE;
                auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) +
                                                        sizeof(DROPFILES));
                memcpy(dst, clip->path.c_str(), clip->path.size() * sizeof(wchar_t));
                dst[clip->path.size()] = 0;
                dst[clip->path.size() + 1] = 0;
                GlobalUnlock(hg);
                SetClipboardData(CF_HDROP, hg);
            }
            CloseClipboard();
            if (web) push_json(this, L"{\"type\":\"toast\",\"text\":\"Clip copied\"}");
        }
        return;
    }
}

// Removes capture metadata, legacy edit projects, and audio stems beside a clip.
void delete_clip_sidecars(const std::wstring& clip_path) {
    std::error_code ec;
    std::filesystem::remove(clip_path + L".apps.json", ec);
    std::filesystem::remove(clip_path + L".edit.json", ec);
    if (clip_path.size() >= 4 &&
        clip_path.compare(clip_path.size() - 4, 4, L".mp4") == 0) {
        std::filesystem::remove_all(clip_path.substr(0, clip_path.size() - 4) + L".stems",
                                    ec);
    }
}

HRESULT EnvCreatedHandler::Invoke(HRESULT error_code, ICoreWebView2Environment* environment) {
    auto* impl =
        api ? static_cast<LibraryWindowImpl*>(api->impl_slot()) : nullptr;
    if (impl) impl->env_pending = false;
    CL_INFO("Library", "env callback fired");
    if (!impl || FAILED(error_code) || !environment) {
        CL_ERROR("Library", "WebView2 environment failed");
        return S_OK;
    }
    impl->env = environment;

    ControllerCreatedHandler* h = new ControllerCreatedHandler();
    h->api = api;
    impl->env->CreateCoreWebView2Controller(impl->hwnd, h);
    return S_OK;
}

HRESULT ControllerCreatedHandler::Invoke(HRESULT error_code,
                                         ICoreWebView2Controller* created) {
    auto* impl =
        api ? static_cast<LibraryWindowImpl*>(api->impl_slot()) : nullptr;
    if (!impl || FAILED(error_code) || !created) {
        CL_ERROR("Library", "WebView2 controller failed");
        return S_OK;
    }
    impl->controller = created;
    impl->controller->get_CoreWebView2(&impl->web);

    Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(impl->controller.As(&controller2)) && controller2) {
        COREWEBVIEW2_COLOR background{};
        background.A = 255;
        background.R = static_cast<BYTE>(kBg & 0xffu);
        background.G = static_cast<BYTE>((kBg >> 8) & 0xffu);
        background.B = static_cast<BYTE>((kBg >> 16) & 0xffu);
        controller2->put_DefaultBackgroundColor(background);
    }

    RECT rc;
    GetClientRect(impl->hwnd, &rc);
    impl->controller->put_Bounds(rc);

    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(impl->web->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->Release();
    }

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path web_dir = std::filesystem::path(exe_path).parent_path() / L"web";

    Microsoft::WRL::ComPtr<ICoreWebView2_3> web3;
    if (SUCCEEDED(impl->web.As(&web3)) && web3) {
        web3->SetVirtualHostNameToFolderMapping(
            L"app.cliplite.local", web_dir.wstring().c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        web3->SetVirtualHostNameToFolderMapping(L"media.cliplite.local", impl->clip_dir.c_str(),
                                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        web3->SetVirtualHostNameToFolderMapping(
            L"thumbs.cliplite.local", cliplite::library::thumbnail_cache_dir().c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    } else {
        CL_ERROR("Library", "ICoreWebView2_3 unavailable (runtime too old)");
    }

    MessageReceivedHandler* mh = new MessageReceivedHandler();
    mh->api = api;
    impl->web->add_WebMessageReceived(mh, &impl->msg_token);
    impl->msg_hooked = true;

    // Surface page JS errors in cliplite.log instead of failing silently.
    impl->web->AddScriptToExecuteOnDocumentCreated(
        L"window.addEventListener('error',function(e){try{window.chrome.webview.postMessage("
        L"JSON.stringify({cmd:'jserror',msg:String(e.message)+' @'+String(e.filename)+':'+"
        L"e.lineno}))}catch(_){}});",
        nullptr);

    impl->ui_ready = false;
    impl->web->Navigate((std::wstring(kAppHost) + L"/index.html").c_str());
    CL_INFO("Library", "webview navigated");
    return S_OK;
}

HRESULT ContentLoadingHandler::Invoke(ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs*) {
    // Intentionally inert: ContentLoading fires BEFORE app.js registers its
    // message listener, so anything posted here would be silently dropped.
    // The page announces real readiness via the 'ready' command.
    return S_OK;
}

HRESULT MessageReceivedHandler::Invoke(ICoreWebView2* sender,
                                       ICoreWebView2WebMessageReceivedEventArgs* args) {
    auto* impl =
        api ? static_cast<LibraryWindowImpl*>(api->impl_slot()) : nullptr;
    if (!impl) return S_OK;
    impl->on_web_message(args);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Public API (thin wrapper over Impl stored in legacy void* slots).
// ---------------------------------------------------------------------------

LibraryWindow::~LibraryWindow() {
    auto* impl = static_cast<LibraryWindowImpl*>(env_);
    if (impl) {
        impl->mic_preview.reset();
        if (impl->msg_hooked && impl->web)
            impl->web->remove_WebMessageReceived(impl->msg_token);
        impl->web.Reset();
        impl->controller.Reset();
        impl->env.Reset();
        delete impl;
    }
    env_ = controller_ = webview_ = msg_token_ = nullptr;
    if (hwnd_) {
        KillTimer(hwnd_, 2);
        DestroyWindow(hwnd_);
    }
}

bool LibraryWindow::create(HINSTANCE hInstance, const std::wstring& clip_dir) {
    hinst_ = hInstance;
    clip_dir_ = clip_dir;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst_;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBg);
    wc.hIcon = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    wc.hIconSm = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    RegisterClassExW(&wc);

    auto* impl = new LibraryWindowImpl();
    impl->api = this;
    impl->hinst = hinst_;
    impl->clip_dir = clip_dir;
    impl->clip_request = clip_request_;
    impl->settings_request = settings_request_;
    env_ = impl;         // impl home slot reused across callbacks
    controller_ = impl;  // all slots alias the same Impl
    webview_ = impl;
    msg_token_ = impl;

    hwnd_ = CreateWindowExW(0, kWindowClass, L"ClipLite", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, hinst_,
                            this);
    if (!hwnd_) {
        delete impl;
        env_ = controller_ = webview_ = msg_token_ = nullptr;
        return false;
    }
    impl->hwnd = hwnd_;
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int scale = static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
        const int work_width = static_cast<int>(monitor.rcWork.right - monitor.rcWork.left);
        const int work_height = static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top);
        const int width = std::min(MulDiv(1440, scale, USER_DEFAULT_SCREEN_DPI), work_width);
        const int height = std::min(MulDiv(900, scale, USER_DEFAULT_SCREEN_DPI), work_height);
        SetWindowPos(hwnd_, nullptr, monitor.rcWork.left + (work_width - width) / 2,
                     monitor.rcWork.top + (work_height - height) / 2, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    style_window_frame(hwnd_);
    impl->ensure_web_started();
    return true;
}

void LibraryWindow::show() {
    if (!hwnd_) return;
    auto* impl = static_cast<LibraryWindowImpl*>(env_);
    if (impl) {
        if (!impl->web && !impl->env_pending) impl->ensure_web_started();
        impl->scan();
        impl->send_clips();
        impl->send_status();
    }
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void LibraryWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool LibraryWindow::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void LibraryWindow::refresh_clips() {
    auto* impl = static_cast<LibraryWindowImpl*>(env_);
    if (!impl || !hwnd_) return;
    if (!IsWindowVisible(hwnd_)) {
        // Hidden: skip the disk scan and Media Foundation probes entirely.
        // Flag stale so the next show()/page-ready handshake refreshes.
        impl->have_clips = true;
        return;
    }
    impl->scan();
    impl->send_clips();
    const WebClip* newest = impl->clips.empty() ? nullptr : &impl->clips.front();
    if (newest && impl->web) {
        push_json(impl, L"{\"type\":\"toast\",\"text\":\"Clip saved Â· " + newest->id +
                            L"\"}");
    }
}

void LibraryWindow::set_clip_request(std::function<void()> fn) {
    clip_request_ = fn;
    if (auto* impl = static_cast<LibraryWindowImpl*>(env_)) impl->clip_request = std::move(fn);
}

void LibraryWindow::attach_settings(cliplite::Settings* settings, const std::string& path,
                                    HWND notify_hwnd, UINT notify_msg) {
    settings_ = settings;
    settings_path_ = path;
    settings_notify_ = notify_hwnd;
    settings_notify_msg_ = notify_msg;
    if (auto* impl = static_cast<LibraryWindowImpl*>(env_)) {
        impl->settings = settings;
        impl->settings_path = path;
        impl->settings_notify = notify_hwnd;
        impl->settings_notify_msg = notify_msg;
    }
}

void LibraryWindow::notify_detected_game(const std::string& exe) {
    if (auto* impl = static_cast<LibraryWindowImpl*>(env_)) impl->detected_exe = exe;
}

void LibraryWindow::open_settings_ui() {
    if (!hwnd_) return;
    if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOW);
    auto* impl = static_cast<LibraryWindowImpl*>(env_);
    if (impl) push_json(impl, L"{\"type\":\"open_settings\"}");
}

void LibraryWindow::set_settings_request(std::function<void()> fn) {
    settings_request_ = fn;
    if (auto* impl = static_cast<LibraryWindowImpl*>(env_)) impl->settings_request = std::move(fn);
}

void LibraryWindow::push_status(bool recording, const std::string& game,
                                const std::string& hotkey) {
    auto* impl = static_cast<LibraryWindowImpl*>(env_);
    if (!impl) return;
    impl->recording = recording;
    impl->game = cliplite::util::utf8_to_wide(game);
    impl->hotkey = cliplite::util::utf8_to_wide(hotkey);
    impl->send_status();
}

LRESULT CALLBACK LibraryWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<LibraryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<LibraryWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_GETMINMAXINFO: {
            auto* bounds = reinterpret_cast<MINMAXINFO*>(lp);
            const UINT dpi = GetDpiForWindow(hwnd);
            const int scale = static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
            bounds->ptMinTrackSize.x = MulDiv(600, scale, USER_DEFAULT_SCREEN_DPI);
            bounds->ptMinTrackSize.y = MulDiv(520, scale, USER_DEFAULT_SCREEN_DPI);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                bounds->ptMinTrackSize.x = std::min(bounds->ptMinTrackSize.x,
                                                  monitor.rcWork.right - monitor.rcWork.left);
                bounds->ptMinTrackSize.y = std::min(bounds->ptMinTrackSize.y,
                                                  monitor.rcWork.bottom - monitor.rcWork.top);
            }
            return 0;
        }
        case WM_SIZE: {
            auto* impl = static_cast<LibraryWindowImpl*>(self->impl_slot());
            if (impl && impl->controller) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                impl->controller->put_Bounds(rc);
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == 2) {
                auto* impl2 = static_cast<LibraryWindowImpl*>(self->impl_slot());
                if (impl2) impl2->poll_mic_levels();
            }
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_CLOSE: {
            auto* implc = static_cast<LibraryWindowImpl*>(self->impl_slot());
            if (implc) {
                implc->shutdown_web();       // free the WebView2 stack while hidden
                implc->mic_preview.reset();  // stop WASAPI mic-preview threads too
            }
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace cliplite::ui
