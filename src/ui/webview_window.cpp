#include "cliplite/ui/library_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"

#include "cliplite/audio/pcm_decode.h"
#include "cliplite/audio/stem_track.h"
#include "cliplite/audio/wasapi_mic.h"
#include "cliplite/audio/wav_peaks.h"

#include "cliplite/library/clip_library.h"
#include "cliplite/library/export_clip.h"
#include "cliplite/library/thumbnails.h"
#include "cliplite/library/stem_mixdown.h"
#include "cliplite/library/video_edit.h"
#include "cliplite/config.h"
#include "cliplite/detection/window_info.h"
#include "cliplite/util/win_utf8.h"
#include "cliplite/log.h"
#include <wrl/client.h>

namespace cliplite::ui {

namespace {

constexpr UINT IDM_PLAY_BASE = 0;

const wchar_t* kWindowClass = L"ClipLiteLibraryWebWindow";
const COLORREF kBg = RGB(11, 13, 16);
const wchar_t* kAppHost = L"https://app.cliplite.local";
const wchar_t* kMediaHost = L"https://media.cliplite.local";
const wchar_t* kThumbsHost = L"https://thumbs.cliplite.local";

constexpr UINT IDD_RENAME = 300;
constexpr UINT IDC_RENAME_EDIT = 301;
constexpr UINT IDD_TRIM = 302;
constexpr UINT IDC_TRIM_START = 310;
constexpr UINT IDC_TRIM_END = 311;
constexpr UINT IDC_TRIM_INFO = 312;

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

struct TrimParams {
    std::wstring path;
    int64_t duration_ms = 0;
};

int get_dlg_int(HWND dlg, int id, int fallback) {
    wchar_t buf[32]{};
    GetDlgItemTextW(dlg, id, buf, _countof(buf));
    wchar_t* endp = nullptr;
    const long v = wcstol(buf, &endp, 10);
    return (endp == buf) ? fallback : static_cast<int>(v);
}

// Parses trim edit boxes as seconds (float allowed) -> milliseconds. Integer
// input keeps working, fixing the old seconds-only truncation (up to 999ms).
int64_t get_dlg_ms(HWND dlg, int id, int64_t fallback_ms) {
    wchar_t buf[32]{};
    GetDlgItemTextW(dlg, id, buf, _countof(buf));
    wchar_t* endp = nullptr;
    const double v = wcstod(buf, &endp);
    if (endp == buf || v < 0) return fallback_ms;
    // Round half away from zero: truncation turns typed "0.1" into 99ms.
    return static_cast<int64_t>(std::llround(v * 1000.0));
}

// Shared render-option parsers for edit_render / project_render (one place,
// identical behavior). "excluded" is pid,pid (mute); "gains" is pid:gain.
// Gains clamp to 0..2; unparsable gains mute (fail-safe, never blast).
void parse_render_gains(const std::wstring& json, std::map<uint32_t, float>& out) {
    out.clear();
    std::wstring list;
    if (extract_field(json, L"excluded", list) && !list.empty()) {
        size_t p = 0;
        while (p < list.size()) {
            const size_t comma = list.find(L',', p);
            const std::wstring one =
                comma == std::wstring::npos ? list.substr(p) : list.substr(p, comma - p);
            p = comma == std::wstring::npos ? list.size() : comma + 1;
            if (one.empty()) continue;
            const unsigned long pid = wcstoul(one.c_str(), nullptr, 10);
            if (pid > 0 && pid <= UINT32_MAX) out[static_cast<uint32_t>(pid)] = 0.f;
        }
    }
    std::wstring glist;
    if (extract_field(json, L"gains", glist) && !glist.empty()) {
        size_t p = 0;
        while (p < glist.size()) {
            const size_t comma = glist.find(L',', p);
            const std::wstring one =
                comma == std::wstring::npos ? glist.substr(p) : glist.substr(p, comma - p);
            p = comma == std::wstring::npos ? glist.size() : comma + 1;
            if (one.empty()) continue;
            const size_t colon = one.find(L':');
            if (colon == std::wstring::npos) continue;
            const unsigned long pid = wcstoul(one.substr(0, colon).c_str(), nullptr, 10);
            if (pid == 0 || pid > UINT32_MAX) continue;
            const double g = wcstod(one.substr(colon + 1).c_str(), nullptr);
            if (!(g > 0.0)) {
                out[static_cast<uint32_t>(pid)] = 0.f;
            } else if (g < 2.0) {
                out[static_cast<uint32_t>(pid)] = static_cast<float>(g);
            } else {
                out[static_cast<uint32_t>(pid)] = 2.f;
            }
        }
    }
}

// Text overlays for project_render. Wire format per text (fields \x1F,
// records \x1E): content, gridX 0..2, gridY 0..2, size S/M/L, color #rrggbb,
// align left/center/right, start_ms, end_ms. Malformed records are skipped;
// overlong content is dropped (never truncated mid-character silently... it
// is truncated, but only past 200 chars, and logged).
void parse_texts(const std::wstring& json, std::vector<cliplite::library::EditText>& out) {
    out.clear();
    std::wstring list;
    if (!extract_field(json, L"texts", list) || list.empty()) return;
    static const wchar_t* kFamilies[] = {L"Segoe UI", L"Arial", L"Times New Roman",
                                         L"Courier New", L"Impact"};
    size_t p = 0;
    while (p < list.size()) {
        const size_t rs = list.find(L'\x1E', p);
        const std::wstring rec =
            rs == std::wstring::npos ? list.substr(p) : list.substr(p, rs - p);
        p = rs == std::wstring::npos ? list.size() : rs + 1;
        if (rec.empty()) continue;
        std::vector<std::wstring> f;
        size_t q = 0;
        while (q <= rec.size() && f.size() <= 8) {
            const size_t fs = rec.find(L'\x1F', q);
            f.push_back(fs == std::wstring::npos ? rec.substr(q) : rec.substr(q, fs - q));
            if (fs == std::wstring::npos) break;
            q = fs + 1;
        }
        if ((f.size() != 8 && f.size() != 9) || f[0].empty() || f[0].size() > 200)
            continue;
        cliplite::library::EditText t;
        t.text = f[0];
        if (f.size() == 9) {
            for (const wchar_t* fam : kFamilies) {
                if (f[8] == fam) {
                    t.family = fam;
                    break;
                }
            }
        }
        const int gx = _wtoi(f[1].c_str());
        const int gy = _wtoi(f[2].c_str());
        if (gx < 0 || gx > 2 || gy < 0 || gy > 2) continue;
        t.x = gx == 0 ? 0.25f : (gx == 1 ? 0.5f : 0.75f);
        t.y = gy == 0 ? 0.25f : (gy == 1 ? 0.5f : 0.75f);
        t.size_frac = f[3] == L"S" ? 0.06f : (f[3] == L"L" ? 0.13f : 0.09f);
        unsigned rgb = 0;
        if (f[4].size() == 7 && f[4][0] == L'#' &&
            swscanf_s(f[4].c_str() + 1, L"%x", &rgb) == 1) {
            t.r = static_cast<uint8_t>((rgb >> 16) & 0xFF);
            t.g = static_cast<uint8_t>((rgb >> 8) & 0xFF);
            t.b = static_cast<uint8_t>(rgb & 0xFF);
        }
        t.align = f[5] == L"left" ? cliplite::library::TextAlign::Left
                  : f[5] == L"right"
                      ? cliplite::library::TextAlign::Right
                      : cliplite::library::TextAlign::Center;
        t.start_ms = _wtoi64(f[6].c_str());
        t.end_ms = _wtoi64(f[7].c_str());
        if (t.start_ms < 0) t.start_ms = 0;
        if (t.end_ms <= t.start_ms) continue;
        out.push_back(std::move(t));
    }
}

// Crop + blur sections of edit_render/project_render. Reads crop/cx/cy/cw/ch
// and blurs from json into eo (crop probing via a throwaway reader).
void parse_crop_blur(const std::wstring& json, const std::wstring& clip_path,
                     cliplite::library::EditOptions& eo) {
    std::wstring cropm, blurs;
    extract_field(json, L"crop", cropm);
    if (cropm == L"custom") {
        eo.crop_x = extract_int(json, L"cx", 0);
        eo.crop_y = extract_int(json, L"cy", 0);
        eo.crop_w = extract_int(json, L"cw", 0);
        eo.crop_h = extract_int(json, L"ch", 0);
    } else if (cropm != L"full") {
        // Fit a centered crop of the requested aspect over the source.
        UINT32 sw = 0, sh = 0;
        Microsoft::WRL::ComPtr<IMFSourceReader> r;
        if (SUCCEEDED(MFStartup(MF_VERSION))) {
            if (SUCCEEDED(MFCreateSourceReaderFromURL(clip_path.c_str(), nullptr, &r))) {
                Microsoft::WRL::ComPtr<IMFMediaType> t;
                if (SUCCEEDED(r->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                                    &t)))
                    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &sw, &sh);
                r.Reset();
            }
            MFShutdown();
        }
        double target = cropm == L"16:9" ? 16.0 / 9
                        : cropm == L"1:1" ? 1.0
                        : cropm == L"9:16" ? 9.0 / 16
                        : cropm == L"4:5" ? 4.0 / 5
                        : cropm == L"4:3" ? 4.0 / 3
                                           : 0.0;
        if (target > 0 && sw > 0 && sh > 0) {
            int w = sw, h = static_cast<int>(w / target);
            if (h > static_cast<int>(sh)) {
                h = sh;
                w = static_cast<int>(h * target);
            }
            eo.crop_x = (static_cast<int>(sw) - w) / 2;
            eo.crop_y = (static_cast<int>(sh) - h) / 2;
            eo.crop_w = w & ~1;
            eo.crop_h = h & ~1;
        }
    }

    extract_field(json, L"blurs", blurs);
    size_t pos = 0;
    while (pos < blurs.size()) {
        const size_t semi = blurs.find(L';', pos);
        const std::wstring one =
            semi == std::wstring::npos ? blurs.substr(pos) : blurs.substr(pos, semi - pos);
        pos = semi == std::wstring::npos ? blurs.size() : semi + 1;
        int vals[4] = {30, 30, 40, 20};
        int idx = 0;
        size_t p2 = 0;
        while (idx < 4 && p2 < one.size()) {
            const size_t comma = one.find(L',', p2);
            vals[idx++] = _wtoi(one.substr(p2, comma - p2).c_str());
            if (comma == std::wstring::npos) break;
            p2 = comma + 1;
        }
        if (idx == 4) {
            cliplite::library::EditBlur eb;
            eb.x = vals[0] / 100.0f;
            eb.y = vals[1] / 100.0f;
            eb.w = std::max(2, vals[2]) / 100.0f;
            eb.h = std::max(2, vals[3]) / 100.0f;
            eo.blurs.push_back(eb);
        }
    }
}

std::wstring unique_trim_path(const std::filesystem::path& src) {
    std::filesystem::path out = src.parent_path() / (src.stem().wstring() + L"_trimmed.mp4");
    int n = 1;
    while (std::filesystem::exists(out)) {
        out = src.parent_path() /
              (src.stem().wstring() + L"_trimmed_" + std::to_wstring(n++) + L".mp4");
        if (n > 99) break;
    }
    return out.wstring();
}

INT_PTR CALLBACK trim_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        auto* tp = reinterpret_cast<TrimParams*>(lp);
        wchar_t buf[32]{};
        swprintf_s(buf, L"%.3f", static_cast<double>(tp->duration_ms) / 1000.0);
        SetDlgItemTextW(dlg, IDC_TRIM_START, L"0");
        SetDlgItemTextW(dlg, IDC_TRIM_END, buf);
        swprintf_s(buf, L"Clip length: %.1f s", static_cast<double>(tp->duration_ms) / 1000.0);
        SetDlgItemTextW(dlg, IDC_TRIM_INFO, buf);
        SetFocus(GetDlgItem(dlg, IDC_TRIM_START));
        return FALSE;
    }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) {
            auto* tp = reinterpret_cast<TrimParams*>(GetWindowLongPtrW(dlg, DWLP_USER));
            const int64_t start_ms = get_dlg_ms(dlg, IDC_TRIM_START, -1);
            int64_t end_ms = get_dlg_ms(dlg, IDC_TRIM_END, -1);
            if (!tp || start_ms < 0 || end_ms < 0 || end_ms <= start_ms ||
                end_ms > tp->duration_ms + 1) {
                if (!(tp && end_ms > tp->duration_ms &&
                      end_ms <= tp->duration_ms + 1)) {
                    MessageBoxW(dlg, L"Enter a valid range within the clip length.", L"Trim",
                                MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                end_ms = tp->duration_ms;
            }
            const std::filesystem::path src(tp->path);
            const std::wstring out = unique_trim_path(src);
            if (cliplite::library::export_trimmed(tp->path, out, start_ms,
                                                  end_ms)) {
                MessageBoxW(dlg,
                            (L"Saved " + std::filesystem::path(out).filename().wstring()).c_str(),
                            L"Trim", MB_OK | MB_ICONINFORMATION);
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxW(dlg, L"Export failed. See cliplite.log for details.", L"Trim",
                            MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    if (msg == WM_CLOSE) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
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

    // Background render job (one at a time).
    std::thread edit_thread;
    std::atomic<int> edit_pct{0};
    std::atomic<bool> edit_done{false};
    std::atomic<bool> edit_ok{false};
    std::atomic<bool> edit_cancel{false};
    std::wstring edit_out_name;
    // Background app-audio mixdown job (one at a time, independent of edits).
    std::thread mix_thread;
    std::atomic<bool> mix_done{false};
    std::atomic<bool> mix_ok{false};
    std::wstring mix_out_name;
    // Background trim job (passthrough copy; independent of edits/mixes).
    std::thread trim_thread;
    std::atomic<bool> trim_done{false};
    std::atomic<bool> trim_ok{false};
    std::wstring trim_out_name;
    // Background editor-data job (filmstrip + per-app peaks; no output file).
    std::thread ed_thread;
    std::atomic<bool> ed_done{false};
    std::wstring ed_json;
    // Mixed stem PCM backing opts.mix_pcm for unified edit renders.
    std::vector<float> mix_pcm_store_;
    bool env_pending = false;

    // Mic level preview (popover).
    std::unique_ptr<audio::MicPreview> mic_preview;
    std::mutex lvl_mu;
    std::map<std::wstring, int> latest_levels;

    void start_edit_job(const std::wstring& input, const cliplite::library::EditOptions& opts);
    void start_project_job(
        const std::wstring& input,
        const std::vector<cliplite::library::ProjectSegment>& segments,
        const cliplite::library::EditOptions& opts);
    void poll_edit_job();
    void send_sources();
    void start_mix_job(const std::wstring& input, const std::set<uint32_t>& excluded);
    void start_trim_job(const std::wstring& input, int64_t start_ms, int64_t end_ms);
    void start_editor_data_job(const std::wstring& id, const std::wstring& clip_path);
    bool ensure_web_started();
    void shutdown_web();
    void scan();
    void send_clips();
    void send_status();
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

void LibraryWindowImpl::start_edit_job(const std::wstring& input,
                                       const cliplite::library::EditOptions& opts) {
    if (edit_thread.joinable()) return;  // a render is already running

    std::filesystem::path src(input);
    std::wstring out = (src.parent_path() / (src.stem().wstring() + L"_edited.mp4")).wstring();
    int n = 1;
    while (std::filesystem::exists(out)) {
        out = (src.parent_path() /
               (src.stem().wstring() + L"_edited_" + std::to_wstring(n++) + L".mp4"))
                  .wstring();
        if (n > 99) break;
    }
    edit_out_name = std::filesystem::path(out).filename().wstring();
    edit_done = false;
    edit_ok = false;
    edit_pct = 0;
    edit_cancel = false;

    const std::wstring in = input;
    const std::wstring outp = out;
    edit_thread = std::thread([this, in, outp, opts] {
        bool ok = false;
        try {
            ok = cliplite::library::edit_video(
                in, outp, opts, [this](int pct) {
                    edit_pct = pct;
                    return !edit_done.load() && !edit_cancel.load();
                });
        } catch (const std::exception& ex) {
            CL_ERROR("Library", std::string("edit job threw: ") + ex.what());
        } catch (...) {
            CL_ERROR("Library", "edit job threw unknown exception");
        }
        edit_ok = ok && !edit_cancel.load();
        edit_done = true;
    });
}

void LibraryWindowImpl::start_project_job(
    const std::wstring& input,
    const std::vector<cliplite::library::ProjectSegment>& segments,
    const cliplite::library::EditOptions& opts) {
    if (edit_thread.joinable()) return;  // shares the edit job slot + edit_done

    std::filesystem::path src(input);
    std::wstring out = (src.parent_path() / (src.stem().wstring() + L"_edited.mp4")).wstring();
    int n = 1;
    while (std::filesystem::exists(out)) {
        out = (src.parent_path() /
               (src.stem().wstring() + L"_edited_" + std::to_wstring(n++) + L".mp4"))
                  .wstring();
        if (n > 99) break;
    }
    edit_out_name = std::filesystem::path(out).filename().wstring();
    edit_done = false;
    edit_ok = false;
    edit_pct = 0;
    edit_cancel = false;

    const std::wstring outp = out;
    // mix_pcm_store_ must outlive the render: opts.mix_pcm is non-owning.
    // It is only reset at the top of edit_render/project_render, both of
    // which refuse to run while edit_thread is joinable.
    edit_thread = std::thread([this, outp, segments, opts] {
        bool ok = false;
        try {
            ok = cliplite::library::edit_project_video(
                segments, outp, opts, [this](int pct) {
                    edit_pct = pct;
                    return !edit_done.load() && !edit_cancel.load();
                });
        } catch (const std::exception& ex) {
            CL_ERROR("Library", std::string("project job threw: ") + ex.what());
        } catch (...) {
            CL_ERROR("Library", "project job threw unknown exception");
        }
        edit_ok = ok && !edit_cancel.load();
        edit_done = true;
    });
}

void LibraryWindowImpl::start_mix_job(const std::wstring& input,
                                      const std::set<uint32_t>& excluded) {
    if (mix_thread.joinable()) return;  // a mixdown is already running

    std::filesystem::path src(input);
    std::wstring out = (src.parent_path() / (src.stem().wstring() + L"_clean.mp4")).wstring();
    int n = 1;
    while (std::filesystem::exists(out)) {
        out = (src.parent_path() /
               (src.stem().wstring() + L"_clean_" + std::to_wstring(n++) + L".mp4"))
                  .wstring();
        if (n > 99) break;
    }
    mix_out_name = std::filesystem::path(out).filename().wstring();
    mix_done = false;
    mix_ok = false;

    const std::wstring in = input;
    const std::wstring outp = out;
    mix_thread = std::thread([this, in, outp, excluded] {
        try {
            mix_ok = cliplite::library::export_without_apps(in, outp, excluded);
        } catch (const std::exception& ex) {
            CL_ERROR("Library", std::string("mix job threw: ") + ex.what());
            mix_ok = false;
        } catch (...) {
            CL_ERROR("Library", "mix job threw unknown exception");
            mix_ok = false;
        }
        mix_done = true;
    });
}

void LibraryWindowImpl::start_trim_job(const std::wstring& input, int64_t start_ms,
                                       int64_t end_ms) {
    if (trim_thread.joinable()) return;  // a trim is already running

    std::filesystem::path src(input);
    std::wstring out =
        (src.parent_path() / (src.stem().wstring() + L"_trimmed.mp4")).wstring();
    int n = 1;
    while (std::filesystem::exists(out)) {
        out = (src.parent_path() /
               (src.stem().wstring() + L"_trimmed_" + std::to_wstring(n++) + L".mp4"))
                  .wstring();
        if (n > 99) break;
    }
    trim_out_name = std::filesystem::path(out).filename().wstring();
    trim_done = false;
    trim_ok = false;

    const std::wstring in = input;
    const std::wstring outp = out;
    trim_thread = std::thread([this, in, outp, start_ms, end_ms] {
        try {
            // Clamp to the probed duration like the classic trim dialog does.
            int64_t dur = cliplite::library::ClipLibrary::probe_duration_ms(in);
            int64_t end = end_ms;
            if (dur > 0 && end > dur) end = dur;
            trim_ok = end > start_ms &&
                      cliplite::library::export_trimmed(in, outp, start_ms, end);
        } catch (const std::exception& ex) {
            CL_ERROR("Library", std::string("trim job threw: ") + ex.what());
            trim_ok = false;
        } catch (...) {
            CL_ERROR("Library", "trim job threw unknown exception");
            trim_ok = false;
        }
        trim_done = true;
    });
}

// Editor project autosave: opaque JSON blob the page owns (clips, gains,
// regions, crop), stored beside the clip. Native only persists bytes (capped,
// brace-checked); the page validates and clamps on restore. Never fails a job.
namespace {
constexpr size_t kEditProjectCap = 256 * 1024;

std::wstring edit_project_path(const std::wstring& clip_path) {
    return clip_path + L".edit.json";
}

bool write_edit_project(const std::wstring& clip_path, const std::string& utf8_body) {
    if (utf8_body.empty() || utf8_body.size() > kEditProjectCap) return false;
    if (utf8_body.front() != '{') return false;
    std::ofstream f(std::filesystem::path(clip_path + L".edit.json"),
                    std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(utf8_body.data(), static_cast<std::streamsize>(utf8_body.size()));
    f.close();
    if (!f) {
        DeleteFileW((clip_path + L".edit.json").c_str());
        return false;
    }
    return true;
}

std::string read_edit_project(const std::wstring& clip_path) {
    std::ifstream f(std::filesystem::path(clip_path + L".edit.json"),
                    std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto n = f.tellg();
    if (n <= 0 || n > static_cast<std::streamoff>(kEditProjectCap)) return {};
    std::string body(static_cast<size_t>(n), '\0');
    f.seekg(0);
    f.read(body.data(), n);
    if (!f || body.empty() || body.front() != '{') return {};
    return body;
}
}  // namespace

void LibraryWindowImpl::start_editor_data_job(const std::wstring& id,
                                              const std::wstring& clip_path) {
    if (ed_thread.joinable()) return;  // a data job is already running
    ed_done = false;
    ed_json.clear();
    ed_thread = std::thread([this, id, clip_path] {
        std::wstring js = L"{\"type\":\"editor_data\",\"id\":" + json_str(id);
        const int64_t dur =
            cliplite::library::ClipLibrary::probe_duration_ms(clip_path);
        js += L",\"dur_ms\":" + std::to_wstring(dur > 0 ? dur : 0);
        // Filmstrip: 12 seeks spread across the clip (disk-cached per frame).
        js += L",\"film\":[";
        if (dur > 0) {
            for (int i = 0; i < 12; ++i) {
                const int64_t t = dur * (i * 2 + 1) / 24;
                HBITMAP bmp = cliplite::library::generate_thumbnail_at(clip_path, 160, 90, t);
                if (bmp) DeleteObject(bmp);
                const std::wstring file =
                    cliplite::library::cached_thumbnail_file_at(clip_path, 160, 90, t);
                if (!file.empty()) {
                    const auto slash = file.find_last_of(L"\\/");
                    const std::wstring name =
                        (slash == std::wstring::npos) ? file : file.substr(slash + 1);
                    if (i) js += L",";
                    js += json_str(std::wstring(kThumbsHost) + L"/" + name);
                }
            }
        }
        js += L"],\"tracks\":[";
        cliplite::library::StemClipInfo info;
        if (cliplite::library::read_stem_sidecar(clip_path, &info)) {
            bool first = true;
            auto groups = cliplite::audio::group_snapshot_paths(
                cliplite::util::wide_to_utf8(info.stem_dir));
            for (const auto& [pid, exe] : info.apps) {
                const auto known = info.has_stem.find(pid);
                const bool removable = known != info.has_stem.end() && known->second;
                // Per-app peaks across its k-files (streaming, O(buckets) RAM).
                std::map<uint32_t, std::string> files;
                for (const auto& [k, m] : groups) {
                    const auto it = m.find(pid);
                    if (it != m.end()) files[k] = it->second;
                }
                const auto peaks = cliplite::audio::concat_pid_peaks(files, 120);
                if (!first) js += L",";
                first = false;
                js += L"{\"pid\":" + std::to_wstring(pid) + L",\"exe\":" +
                      json_str(cliplite::util::utf8_to_wide(exe)) +
                      (removable ? L",\"removable\":true" : L",\"removable\":false") +
                      L",\"peaks\":[";
                for (size_t i = 0; i < peaks.size(); ++i) {
                    if (i) js += L",";
                    wchar_t b[16]{};
                    swprintf_s(b, L"%.3f", static_cast<double>(peaks[i]));
                    js += b;
                }
                js += L"]}";
            }
        }
        js += L"]";
        // Autosaved project, if any: embedded raw (page validates + clamps).
        const std::string saved = read_edit_project(clip_path);
        if (!saved.empty() && saved.back() == '}') {
            js += L",\"saved\":";
            js += cliplite::util::utf8_to_wide(saved);
        } else {
            js += L",\"saved\":null";
        }
        js += L"}";  // closes the editor_data object opened at thread start
        ed_json = js;
        ed_done = true;
    });
}

void LibraryWindowImpl::poll_edit_job() {
    // Flush mic preview levels to the popover (if any accumulated).
    if (web && ui_ready) {
        std::map<std::wstring, int> levels;
        {
            std::lock_guard<std::mutex> lk(lvl_mu);
            levels.swap(latest_levels);
        }
        if (!levels.empty()) {
            std::wstring js = L"{\"type\":\"miclevels\",\"levels\":[";
            bool first = true;
            for (auto& [id, pct] : levels) {
                if (!first) js += L",";
                first = false;
                js += L"{\"i\":" + json_str(id) + L",\"p\":" + std::to_wstring(pct) + L"}";
            }
            js += L"]}";
            push_json(this, js);
        }
    }
    if (!edit_done.load()) {
        if (edit_pct > 0 && edit_pct < 100)
            push_json(this, L"{\"type\":\"progress\",\"pct\":" + std::to_wstring(edit_pct.load()) +
                                L"}");
    } else {
        if (edit_thread.joinable()) edit_thread.join();
        const bool was_cancel = edit_cancel.load();
        edit_cancel = false;
        if (was_cancel) {
            push_json(this, L"{\"type\":\"toast\",\"text\":\"Render cancelled\"}");
            push_json(this, L"{\"type\":\"edit_done\",\"ok\":false}");
        } else {
            push_json(this,
                      edit_ok
                          ? (L"{\"type\":\"edit_done\",\"ok\":true,\"name\":" +
                             json_str(edit_out_name) + L"}")
                          : L"{\"type\":\"edit_done\",\"ok\":false}");
        }
        if (edit_ok && !was_cancel) {
            scan();
            send_clips();
        }
        edit_pct = 0;
        edit_done = false;
    }
    if (mix_done.load()) {
        if (mix_thread.joinable()) mix_thread.join();
        push_json(this,
                  mix_ok
                      ? (L"{\"type\":\"app_audio_done\",\"ok\":true,\"name\":" +
                         json_str(mix_out_name) + L"}")
                      : L"{\"type\":\"app_audio_done\",\"ok\":false}");
        if (mix_ok) {
            scan();
            send_clips();
        }
        mix_done = false;
    }
    if (trim_done.load()) {
        if (trim_thread.joinable()) trim_thread.join();
        push_json(this,
                  trim_ok
                      ? (L"{\"type\":\"trim_done\",\"ok\":true,\"name\":" +
                         json_str(trim_out_name) + L"}")
                      : L"{\"type\":\"trim_done\",\"ok\":false}");
        if (trim_ok) {
            scan();
            send_clips();
        }
        trim_done = false;
    }
    if (ed_done.load()) {
        if (ed_thread.joinable()) ed_thread.join();
        if (web && ui_ready && !ed_json.empty()) push_json(this, ed_json);
        ed_json.clear();
        ed_done = false;
    }
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

// Deletes a clip's editor sidecars next to it (apps/stems/edit-project).
// Best-effort; the recorder never reads these back.
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
    if (cmd == L"editor_data") {
        if (!clip) return;
        // Stale job results are dropped by id on the page; just don't stack up.
        if (!ed_thread.joinable()) start_editor_data_job(id, clip->path);
        return;
    }
    if (cmd == L"edit_render") {
        if (!clip) return;
        if (edit_thread.joinable()) {
            push_json(this, L"{\"type\":\"toast\",\"text\":\"A render is already running\"}");
            return;
        }

        cliplite::library::EditOptions eo;
        // llround, not truncation: (int64_t)(0.1*1000.0) is 99 (0.1 is not
        // binary-exact), silently shortening every decimal-second trim.
        const double ss = wcstod(extract_num(json, L"start_s").c_str(), nullptr);
        const double es = wcstod(extract_num(json, L"end_s").c_str(), nullptr);        eo.start_ms = static_cast<int64_t>(std::llround(ss * 1000.0));
        eo.end_ms = es > 0 ? static_cast<int64_t>(std::llround(es * 1000.0)) : 0;
        // Per-app gains for the unified render (empty = original audio).
        std::map<uint32_t, float> render_gains;
        parse_render_gains(json, render_gains);
        mix_pcm_store_.clear();
        // Remixing is needed when anything is muted OR any gain differs.
        bool need_mix = false;
        bool all_muted = !render_gains.empty();
        for (const auto& [pid, g] : render_gains) {
            (void)pid;
            if (!(g >= 0.999f && g <= 1.001f)) {
                need_mix = true;
            }
            if (g > 0.f) all_muted = false;
        }
        if (all_muted) {
            // Explicit silence requested: video-only, never the original mix.
            eo.force_video_only = true;
            push_json(this,
                      L"{\"type\":\"toast\",\"text\":\"All apps muted \u2014 rendering "
                      L"video-only\"}");
        } else if (need_mix) {
            // Unified path: mix the window's stems with gains, then slice
            // to the edit range. Falls back to original audio when no usable
            // stems exist (old clips), with an honest toast.
            cliplite::library::StemClipInfo info;
            bool usable = false;
            if (cliplite::library::read_stem_sidecar(clip->path, &info) &&
                !info.stem_dir.empty()) {
                // Stream-mixed per k-slice: only the output lives in RAM.
                auto mixed = cliplite::audio::mix_snapshot_streaming_gains(
                    cliplite::util::wide_to_utf8(info.stem_dir), info.has_stem,
                    render_gains, info.head_trim_ms, info.tail_trim_ms);
                if (!mixed.empty()) {
                    // mixed is interleaved stereo covering the clip from 0.
                    const int64_t dur =
                        cliplite::library::ClipLibrary::probe_duration_ms(clip->path);
                    const int64_t e = (eo.end_ms > 0 ? eo.end_ms : (dur > 0 ? dur : 0));
                    size_t drop =
                        static_cast<size_t>(std::max<int64_t>(0, eo.start_ms)) * 48 * 2;
                    size_t keep =
                        static_cast<size_t>(std::max<int64_t>(0, e - eo.start_ms)) * 48 * 2;
                    drop -= drop % 2;
                    keep -= keep % 2;
                    if (drop < mixed.size() && keep > 0) {
                        const size_t n = std::min(keep, mixed.size() - drop);
                        mix_pcm_store_.assign(mixed.begin() + drop, mixed.begin() + drop + n);
                    }
                    usable = !mix_pcm_store_.empty();
                }
            }
            if (usable) {
                eo.mix_pcm = &mix_pcm_store_;
            } else {
                push_json(this,
                          L"{\"type\":\"toast\",\"text\":\"No separate audio tracks for "
                          L"this clip \u2014 keeping original audio\"}");
            }
        }

        parse_crop_blur(json, clip->path, eo);

        start_edit_job(clip->path, eo);
        return;
    }
    if (cmd == L"project_render") {
        if (!clip) return;
        if (edit_thread.joinable()) {
            push_json(this, L"{\"type\":\"toast\",\"text\":\"A render is already running\"}");
            return;
        }
        // Multi-segment timeline: "segments" is "si,s,e;..." (si = source
        // index, 0 = this clip; legacy "s,e" records mean si 0). "sources" is
        // "clipId;..." for si >= 1, resolved against the library so paths can
        // never escape the clip folder.
        struct SegIn {
            std::wstring path;
            int64_t s = 0, e = 0;
        };
        std::vector<SegIn> seg_ins;
        {
            std::vector<std::wstring> src_paths{clip->path};
            std::wstring sources;
            if (extract_field(json, L"sources", sources) && !sources.empty()) {
                size_t p = 0;
                while (p < sources.size()) {
                    const size_t semi = sources.find(L';', p);
                    const std::wstring one = semi == std::wstring::npos
                                                 ? sources.substr(p)
                                                 : sources.substr(p, semi - p);
                    p = semi == std::wstring::npos ? sources.size() : semi + 1;
                    if (one.empty()) continue;
                    const WebClip* other = find(one);
                    if (!other) {
                        push_json(this, L"{\"type\":\"edit_done\",\"ok\":false}");
                        return;
                    }
                    src_paths.push_back(other->path);
                }
            }
            std::wstring segs;
            if (!extract_field(json, L"segments", segs) || segs.empty()) {
                push_json(this, L"{\"type\":\"edit_done\",\"ok\":false}");
                return;
            }
            size_t p = 0;
            while (p < segs.size()) {
                const size_t semi = segs.find(L';', p);
                const std::wstring one =
                    semi == std::wstring::npos ? segs.substr(p) : segs.substr(p, semi - p);
                p = semi == std::wstring::npos ? segs.size() : semi + 1;
                if (one.empty()) continue;
                // Split into comma parts: [si,]s,e.
                std::vector<std::wstring> parts;
                size_t q = 0;
                while (q <= one.size()) {
                    const size_t comma = one.find(L',', q);
                    parts.push_back(comma == std::wstring::npos ? one.substr(q)
                                                                : one.substr(q, comma - q));
                    if (comma == std::wstring::npos) break;
                    q = comma + 1;
                }
                size_t si = 0;
                int64_t s = 0, e = 0;
                if (parts.size() == 3) {
                    si = static_cast<size_t>(_wtoi64(parts[0].c_str()));
                    s = _wtoi64(parts[1].c_str());
                    e = _wtoi64(parts[2].c_str());
                } else if (parts.size() == 2) {
                    s = _wtoi64(parts[0].c_str());
                    e = _wtoi64(parts[1].c_str());
                } else {
                    continue;
                }
                if (e <= s || s < 0 || si >= src_paths.size()) continue;
                SegIn in;
                in.path = src_paths[si];
                in.s = s;
                in.e = e;
                seg_ins.push_back(std::move(in));
            }
        }
        if (seg_ins.empty()) {
            push_json(this, L"{\"type\":\"edit_done\",\"ok\":false}");
            return;
        }
        // Per-segment playback speed ("speeds" is "sp0;sp1...", 0.25..4.0).
        std::vector<double> seg_speeds;
        {
            std::wstring slist;
            if (extract_field(json, L"speeds", slist) && !slist.empty()) {
                size_t p2 = 0;
                while (p2 < slist.size()) {
                    const size_t semi = slist.find(L';', p2);
                    const std::wstring one =
                        semi == std::wstring::npos ? slist.substr(p2) : slist.substr(p2, semi - p2);
                    p2 = semi == std::wstring::npos ? slist.size() : semi + 1;
                    const double v = wcstod(one.c_str(), nullptr);
                    seg_speeds.push_back((v >= 0.25 && v <= 4.0) ? v : 1.0);
                }
            }
        }
        while (seg_speeds.size() < seg_ins.size()) seg_speeds.push_back(1.0);

        cliplite::library::EditOptions eo;
        std::map<uint32_t, float> render_gains;
        parse_render_gains(json, render_gains);
        parse_crop_blur(json, clip->path, eo);
        parse_texts(json, eo.texts);

        // Concatenated mix on the OUTPUT timeline, one span per segment:
        // main-clip segments slice the stem mix (per-app gains); import
        // segments decode their original audio. Spans without audio get
        // exact-length silence so A/V can never drift. Falls back exactly
        // like edit_render when nothing usable exists.
        mix_pcm_store_.clear();
        bool need_mix = false;
        for (const auto& [pid, g] : render_gains) {
            (void)pid;
            if (!(g >= 0.999f && g <= 1.001f)) {
                need_mix = true;
                break;
            }
        }
        if (!need_mix) {
            for (double sp : seg_speeds) {
                if (sp < 0.999 || sp > 1.001) {
                    need_mix = true;
                    break;
                }
            }
        }
        bool any_import = false;
        for (const auto& in : seg_ins) {
            if (in.path != clip->path) {
                any_import = true;
                break;
            }
        }
        // All-muted (every gain explicitly 0): video-only, never original.
        bool all_muted = !render_gains.empty();
        for (const auto& [pid, g] : render_gains) {
            (void)pid;
            if (g > 0.f) {
                all_muted = false;
                break;
            }
        }
        if (all_muted) {
            eo.force_video_only = true;
            push_json(this,
                      L"{\"type\":\"toast\",\"text\":\"All apps muted \u2014 rendering "
                      L"video-only\"}");
        } else if (need_mix || seg_ins.size() > 1 || any_import) {
            // Whole-clip stem mix, sliced per main-clip segment below.
            std::vector<float> stem_mix;
            {
                cliplite::library::StemClipInfo info;
                if (cliplite::library::read_stem_sidecar(clip->path, &info) &&
                    !info.stem_dir.empty()) {
                    stem_mix = cliplite::audio::mix_snapshot_streaming_gains(
                        cliplite::util::wide_to_utf8(info.stem_dir), info.has_stem,
                        render_gains, info.head_trim_ms, info.tail_trim_ms);
                }
            }
            bool have_signal = false;
            for (size_t i = 0; i < seg_ins.size(); ++i) {
                const int64_t s = seg_ins[i].s;
                // Output span of this segment (speed-adjusted, even samples).
                size_t span = static_cast<size_t>(std::max<int64_t>(
                                  0, static_cast<int64_t>(std::llround(
                                         static_cast<double>(seg_ins[i].e - s) /
                                         seg_speeds[i])))) *
                              48 * 2;
                span -= span % 2;
                if (span == 0) continue;
                std::vector<float> sub;
                if (seg_ins[i].path == clip->path && !stem_mix.empty()) {
                    size_t drop = static_cast<size_t>(std::max<int64_t>(0, s)) * 48 * 2;
                    size_t keep = static_cast<size_t>(
                                      std::max<int64_t>(0, seg_ins[i].e - s)) *
                                  48 * 2;
                    drop -= drop % 2;
                    keep -= keep % 2;
                    if (drop < stem_mix.size() && keep > 0) {
                        const size_t n = std::min(keep, stem_mix.size() - drop);
                        sub.assign(stem_mix.begin() + drop, stem_mix.begin() + drop + n);
                    }
                } else {
                    // Imported clip (or main clip without stems): decode the
                    // original audio for this exact range.
                    sub = cliplite::audio::decode_audio_range(seg_ins[i].path, s,
                                                              seg_ins[i].e);
                }
                // Speed, then pad short decodes with silence so the mix
                // covers the output timeline contiguously.
                const double sp = seg_speeds[i];
                if (!sub.empty() && (sp < 0.999 || sp > 1.001)) {
                    sub = cliplite::audio::resample_pcm_linear(sub, 2, sp);
                }
                if (sub.size() != span) sub.resize(span, 0.f);
                for (float f : sub) {
                    if (f != 0.f) {
                        have_signal = true;
                        break;
                    }
                }
                mix_pcm_store_.insert(mix_pcm_store_.end(), sub.begin(), sub.end());
            }
            if (have_signal) {
                eo.mix_pcm = &mix_pcm_store_;
            } else {
                mix_pcm_store_.clear();
                if (seg_ins.size() == 1 && !any_import) {
                    push_json(this,
                              L"{\"type\":\"toast\",\"text\":\"No separate audio tracks for "
                              L"this clip \u2014 keeping original audio\"}");
                } else {
                    push_json(this,
                              L"{\"type\":\"toast\",\"text\":\"No usable audio "
                              L"\u2014 rendering video-only\"}");
                }
            }
        }

        // Aspect canvas fit: letterbox the source window into the crop
        // preset's aspect instead of center-cropping (fill). Only for aspect
        // presets; full/custom keep canvas == source window.
        {
            std::wstring cropm;
            extract_field(json, L"crop", cropm);
            const int fit = extract_int(json, L"canvas_fit", 0);
            int arw = 0, arh = 0;
            if (fit == 1) {
                if (cropm == L"16:9") {
                    arw = 16;
                    arh = 9;
                } else if (cropm == L"1:1") {
                    arw = 1;
                    arh = 1;
                } else if (cropm == L"9:16") {
                    arw = 9;
                    arh = 16;
                } else if (cropm == L"4:5") {
                    arw = 4;
                    arh = 5;
                } else if (cropm == L"4:3") {
                    arw = 4;
                    arh = 3;
                }
            }
            eo.canvas_ar_w = arw;
            eo.canvas_ar_h = arh;
            if (arw > 0 && cropm != L"custom") {
                // Fit needs the FULL source window: undo the fill center-crop
                // parse_crop_blur just resolved (custom crops still fit).
                eo.crop_x = eo.crop_y = eo.crop_w = eo.crop_h = 0;
            }
        }
        // Export targets: res (0=source, else target height), fps (0=source,
        // else 30/60), quality (0 low, 1 medium, 2 high).
        {
            const int res = extract_int(json, L"res", 0);
            eo.out_height = (res == 720 || res == 1080) ? res : 0;
            const int fps = extract_int(json, L"fps", 0);
            eo.out_fps = (fps == 30 || fps == 60) ? fps : 0;
            const int q = extract_int(json, L"quality", 1);
            eo.quality = (q >= 0 && q <= 2) ? q : 1;
        }
        // Fades (ms, output timeline): audio pre-faded on the concatenated
        // mix here; video faded per frame inside the render from eo fields.
        {
            const int64_t fi = static_cast<int64_t>(
                std::llround(wcstod(extract_num(json, L"fade_in_ms").c_str(), nullptr)));
            const int64_t fo = static_cast<int64_t>(
                std::llround(wcstod(extract_num(json, L"fade_out_ms").c_str(), nullptr)));
            eo.fade_in_ms = std::min<int64_t>(std::max<int64_t>(0, fi), 10000);
            eo.fade_out_ms = std::min<int64_t>(std::max<int64_t>(0, fo), 10000);
            if (!mix_pcm_store_.empty() &&
                (eo.fade_in_ms > 0 || eo.fade_out_ms > 0)) {
                cliplite::audio::apply_fade_inout(
                    mix_pcm_store_, 2,
                    static_cast<uint32_t>(
                        static_cast<uint64_t>(eo.fade_in_ms) * 48),
                    static_cast<uint32_t>(
                        static_cast<uint64_t>(eo.fade_out_ms) * 48));
            }
        }

        std::vector<cliplite::library::ProjectSegment> psegs;
        for (size_t i = 0; i < seg_ins.size(); ++i) {
            cliplite::library::ProjectSegment ps;
            ps.input = seg_ins[i].path;
            ps.start_ms = seg_ins[i].s;
            ps.end_ms = seg_ins[i].e;
            ps.volume = 1.0f;  // volume already baked into the sliced mix
            ps.speed = i < seg_speeds.size() ? seg_speeds[i] : 1.0;
            psegs.push_back(std::move(ps));
        }
        start_project_job(clip->path, psegs, eo);
        return;
    }
    if (cmd == L"project_save") {
        if (!clip) return;
        std::wstring body;
        if (extract_field(json, L"project", body) && !body.empty()) {
            // UTF-8 storage so non-ASCII text overlays (emoji/CJK) survive
            // autosave instead of being stripped.
            const int n = WideCharToMultiByte(CP_UTF8, 0, body.c_str(), -1, nullptr, 0,
                                              nullptr, nullptr);
            std::string out;
            if (n > 1) {
                out.assign(static_cast<size_t>(n) - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, body.c_str(), -1, out.data(), n, nullptr,
                                    nullptr);
            }
            if (!write_edit_project(clip->path, out)) {
                CL_WARN("Library", "project autosave failed: " +
                                       cliplite::util::wide_to_utf8(clip->path));
            }
        }
        return;
    }
    if (cmd == L"project_clear") {
        if (!clip) return;
        DeleteFileW(edit_project_path(clip->path).c_str());
        return;
    }
    if (cmd == L"preview_source") {
        // Editor preview switching for imported segments: same media URL the
        // player uses, delivered to the editor video element only.
        const WebClip* target = id.empty() ? nullptr : find(id);
        if (target && web) {
            std::wstring js = L"{\"type\":\"preview_url\",\"id\":" + json_str(id);
            js += L",\"url\":" +
                  json_str(std::wstring(kMediaHost) + L"/" + url_escape_segment(id));
            js += L"}";
            push_json(this, js);
        }
        return;
    }
    if (cmd == L"cancel_render") {
        if (edit_thread.joinable()) {
            edit_cancel = true;
            push_json(this, L"{\"type\":\"toast\",\"text\":\"Cancelling render…\"}");
        }
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
    if (cmd == L"trim") {
        TrimParams tp;
        tp.path = clip->path;
        tp.duration_ms = cliplite::library::ClipLibrary::probe_duration_ms(clip->path);
        if (tp.duration_ms <= 0) {
            MessageBoxW(hwnd, L"Could not determine the clip duration.", L"Trim",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        if (DialogBoxParamW(hinst, MAKEINTRESOURCEW(IDD_TRIM), hwnd, trim_proc,
                            reinterpret_cast<LPARAM>(&tp)) == IDOK) {
            scan();
            send_clips();
            if (web) {
                push_json(this, L"{\"type\":\"toast\",\"text\":\"Trim export finished\"}");
            }
        }
        return;
    }
    if (cmd == L"app_audio") {
        // List every app heard in this clip (all selected = full mix); only
        // stem:true entries are actually removable.
        cliplite::library::StemClipInfo info;
        std::wstring js = L"{\"type\":\"app_audio\",\"id\":" + json_str(id) + L",\"apps\":[";
        if (cliplite::library::read_stem_sidecar(clip->path, &info)) {
            bool first = true;
            for (const auto& [pid, exe] : info.apps) {
                if (!first) js += L",";
                first = false;
                const auto it = info.has_stem.find(pid);
                const bool removable = it != info.has_stem.end() && it->second;
                js += L"{\"pid\":" + std::to_wstring(pid) + L",\"exe\":" +
                      json_str(cliplite::util::utf8_to_wide(exe)) +
                      (removable ? L",\"removable\":true" : L",\"removable\":false") + L"}";
            }
        }
        js += L"]}";
        if (web) push_json(this, js);
        return;
    }
    if (cmd == L"app_audio_render") {
        if (mix_thread.joinable()) {
            push_json(this, L"{\"type\":\"toast\",\"text\":\"A mixdown is already running\"}");
            return;
        }
        std::set<uint32_t> excluded;
        std::wstring list;
        if (extract_field(json, L"excluded", list) && !list.empty()) {
            size_t p = 0;
            while (p < list.size()) {
                const size_t comma = list.find(L',', p);
                const std::wstring one =
                    comma == std::wstring::npos ? list.substr(p) : list.substr(p, comma - p);
                p = comma == std::wstring::npos ? list.size() : comma + 1;
                if (one.empty()) continue;
                const unsigned long pid = wcstoul(one.c_str(), nullptr, 10);
                if (pid > 0 && pid <= UINT32_MAX) excluded.insert(static_cast<uint32_t>(pid));
            }
        }
        if (excluded.empty()) {
            push_json(this,
                      L"{\"type\":\"toast\",\"text\":\"Uncheck an app to remove its sound\"}");
            return;
        }
        start_mix_job(clip->path, excluded);
        push_json(this, L"{\"type\":\"toast\",\"text\":\"Rendering clean mix…\"}");
        return;
    }
    if (cmd == L"trim_render") {
        if (trim_thread.joinable()) {
            push_json(this, L"{\"type\":\"toast\",\"text\":\"A trim is already running\"}");
            return;
        }
        const int64_t start_ms =
            static_cast<int64_t>(wcstod(extract_num(json, L"start_ms").c_str(), nullptr));
        const int64_t end_ms =
            static_cast<int64_t>(wcstod(extract_num(json, L"end_ms").c_str(), nullptr));
        if (start_ms < 0 || end_ms <= start_ms) {
            push_json(this, L"{\"type\":\"trim_done\",\"ok\":false}");
            return;
        }
        start_trim_job(clip->path, start_ms, end_ms);
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

// Deletes a clip's editor sidecars next to it (apps/stems/edit-project).
// Best-effort; the recorder never reads these back.
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
        impl->edit_cancel = true;  // don't block teardown on a long finalize
        if (impl->edit_thread.joinable()) impl->edit_thread.join();
        if (impl->mix_thread.joinable()) impl->mix_thread.join();
        if (impl->trim_thread.joinable()) impl->trim_thread.join();
        if (impl->ed_thread.joinable()) impl->ed_thread.join();
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
                            CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720, nullptr, nullptr, hinst_,
                            this);
    if (!hwnd_) {
        delete impl;
        env_ = controller_ = webview_ = msg_token_ = nullptr;
        return false;
    }
    impl->hwnd = hwnd_;
    SetTimer(hwnd_, 2, 400, nullptr);  // render-job progress polling
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
                if (impl2) impl2->poll_edit_job();
            }
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_CLOSE: {
            auto* implc = static_cast<LibraryWindowImpl*>(self->impl_slot());
            if (implc) {
                implc->shutdown_web();       // free the WebView2 stack while hidden
                implc->mic_preview.reset();  // stop WASAPI mic-preview threads too
                // Shed the working set so closed-but-running shows minimal RAM.
                // Skip while a render job is alive; trimming would just make it
                // soft-fault everything back in on the next progress tick.
                implc->edit_cancel = true;
                if (!implc->edit_thread.joinable() && !implc->mix_thread.joinable() &&
                    !implc->trim_thread.joinable() && !implc->ed_thread.joinable()) {
                    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                             static_cast<SIZE_T>(-1));
                }
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
