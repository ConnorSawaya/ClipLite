#include "cliplite/library/clip_library.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "cliplite/util/win_utf8.h"

namespace cliplite::library {

namespace {

std::string game_from_filename(const std::wstring& stem) {
    const std::string s = cliplite::util::wide_to_utf8(stem);
    const auto us = s.find('_');
    if (us == std::string::npos) return s;
    return s.substr(0, us);
}

// Memoized durations keyed by path + mtime so repeated library refreshes and
// search keystrokes do not reopen every MP4. UI-thread only; mutex for safety.
// NOTE: cache + mutex are shared file-statics so store is visible to lookup.
std::mutex g_duration_mu;
std::unordered_map<std::wstring, int64_t> g_duration_cache;

std::wstring duration_cache_key(const std::wstring& path, bool* ok = nullptr) {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        if (ok) *ok = false;
        return {};
    }
    if (ok) *ok = true;
    const auto key_stamp =
        static_cast<int64_t>(stamp.time_since_epoch().count());
    return path + L"|" + std::to_wstring(key_stamp);
}

int64_t cached_duration_ms(const std::wstring& path) {
    bool ok = false;
    const std::wstring key = duration_cache_key(path, &ok);
    if (!ok) return 0;

    std::lock_guard<std::mutex> lock(g_duration_mu);
    const auto it = g_duration_cache.find(key);
    if (it != g_duration_cache.end()) return it->second;
    return 0;
}

void store_duration_ms(const std::wstring& path, int64_t ms) {
    bool ok = false;
    const std::wstring key = duration_cache_key(path, &ok);
    if (!ok) return;

    std::lock_guard<std::mutex> lock(g_duration_mu);
    g_duration_cache[key] = ms;
    if (g_duration_cache.size() > 512) g_duration_cache.clear();  // crude bound
}

}  // namespace

void ClipLibrary::scan(const std::wstring& directory) {
    clips_.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto& p = entry.path();
        if (p.extension() != L".mp4") continue;

        ClipEntry c;
        c.path = p.wstring();
        c.filename = p.filename().wstring();
        c.game = game_from_filename(p.stem().wstring());
        c.file_size = entry.file_size(ec);
        clips_.push_back(std::move(c));
    }
    // Newest first: compare the timestamp suffix encoded in each filename.
    auto sort_key = [](const ClipEntry& c) {
        const auto us = c.filename.find(L'_');
        return us == std::wstring::npos ? c.filename : c.filename.substr(us + 1);
    };
    std::sort(clips_.begin(), clips_.end(), [&](const ClipEntry& a, const ClipEntry& b) {
        return sort_key(a) > sort_key(b);
    });
}

int64_t ClipLibrary::probe_duration_ms(const std::wstring& path) {
    if (int64_t hit = cached_duration_ms(path); hit > 0) return hit;

    const HRESULT startup = MFStartup(MF_VERSION);
    int64_t ms = 0;

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (SUCCEEDED(hr)) {
        PROPVARIANT var;
        PropVariantInit(&var);
        hr = reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var);
        if (SUCCEEDED(hr) && var.vt == VT_UI8) {
            ms = static_cast<int64_t>(var.uhVal.QuadPart / 10000);
        }
        PropVariantClear(&var);
    }
    reader.Reset();
    if (SUCCEEDED(startup)) MFShutdown();

    if (ms > 0) store_duration_ms(path, ms);
    return ms;
}

}  // namespace cliplite::library
