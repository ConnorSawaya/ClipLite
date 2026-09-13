#include "cliplite/library/audio_peaks.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "cliplite/log.h"

namespace cliplite::library {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

// Mirrors thumbnails.cpp keying (path + write time) so renames/edits refresh.
std::wstring peaks_cache_file(const std::wstring& path, uint32_t buckets) {
    FILETIME ft{};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        GetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
    }
    const std::wstring key =
        path + L"|" + std::to_wstring(static_cast<uint64_t>(ft.dwHighDateTime) << 32 |
                                      ft.dwLowDateTime);
    const size_t hash_value = std::hash<std::wstring>{}(key);
    wchar_t dir[MAX_PATH]{};
    std::wstring base(L".");
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) > 0) base = dir;
    std::error_code ec;
    const std::filesystem::path cache =
        std::filesystem::path(base) / L"ClipLite" / L"thumbs";
    std::filesystem::create_directories(cache, ec);
    wchar_t name[96]{};
    swprintf_s(name, L"%016llX_%u.peaks.json", static_cast<unsigned long long>(hash_value),
               buckets);
    return (cache / name).wstring();
}
}  // namespace

std::vector<float> compute_media_peaks(const std::wstring& path, uint32_t buckets) {
    std::vector<float> peaks;
    if (path.empty() || buckets == 0) return peaks;

    const HRESULT startup = MFStartup(MF_VERSION);
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        if (SUCCEEDED(startup)) MFShutdown();
        return peaks;
    }

    // Duration first (header-only) to size the buckets.
    int64_t dur_hns = 0;
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(reader->GetPresentationAttribute(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
            var.vt == VT_UI8) {
            dur_hns = static_cast<int64_t>(var.uhVal.QuadPart);
        }
        PropVariantClear(&var);
    }
    if (dur_hns <= 0) {
        reader.Reset();
        if (SUCCEEDED(startup)) MFShutdown();
        return peaks;
    }

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), FALSE);
    // Float preferred; PCM16 fallback (same box-dependent gap as the mixdown
    // test documented: some machines lack the converter leg).
    uint32_t bytes_per_sample = 0;
    for (const GUID& sub : {MFAudioFormat_Float, MFAudioFormat_PCM}) {
        Microsoft::WRL::ComPtr<IMFMediaType> mt;
        if (FAILED(MFCreateMediaType(&mt))) break;
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        mt->SetGUID(MF_MT_SUBTYPE, sub);
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                  nullptr, mt.Get()))) {
            bytes_per_sample = (sub == MFAudioFormat_Float) ? 4 : 2;
            break;
        }
    }
    if (bytes_per_sample == 0) {
        reader.Reset();
        if (SUCCEEDED(startup)) MFShutdown();
        return peaks;
    }

    peaks.assign(buckets, 0.f);
    bool saw_any = false;
    for (int guard = 0; guard < 1'000'000; ++guard) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags,
                                &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&data, nullptr, &len)) || !data) continue;
        const uint32_t stride = (bytes_per_sample == 4) ? 4 : 2;
        float peak = 0.f;
        for (DWORD i = 0; i + stride <= len; i += stride) {
            float s = 0.f;
            if (bytes_per_sample == 4) {
                float v = 0.f;
                std::memcpy(&v, data + i, 4);
                s = std::fabs(v);
            } else {
                int16_t v = 0;
                std::memcpy(&v, data + i, 2);
                s = std::fabs(static_cast<float>(v) / 32768.f);
            }
            if (s > peak) peak = s;
        }
        buf->Unlock();
        if (ts < 0) ts = 0;
        const uint32_t b =
            static_cast<uint32_t>(static_cast<uint64_t>(ts) * buckets /
                                  static_cast<uint64_t>(dur_hns));
        if (b < buckets) {
            if (peak > peaks[b]) peaks[b] = peak;
            saw_any = true;
        }
    }
    reader.Reset();
    if (SUCCEEDED(startup)) MFShutdown();
    if (!saw_any) peaks.clear();
    return peaks;
}

std::vector<float> cached_media_peaks(const std::wstring& path, uint32_t buckets) {
    if (path.empty() || buckets == 0) return {};
    try {
        const std::wstring cache = peaks_cache_file(path, buckets);
        {
            std::ifstream in(std::filesystem::path(cache), std::ios::binary);
            if (in) {
                std::string body((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                std::vector<float> out;
                size_t p = 0;
                while (out.size() < buckets && p < body.size()) {
                    while (p < body.size() &&
                           !(body[p] == '-' || body[p] == '.' ||
                             (body[p] >= '0' && body[p] <= '9')))
                        ++p;
                    size_t e = p;
                    while (e < body.size() &&
                           (body[e] == '-' || body[e] == '.' || body[e] == 'e' ||
                            body[e] == 'E' || body[e] == '+' ||
                            (body[e] >= '0' && body[e] <= '9')))
                        ++e;
                    if (e > p) {
                        out.push_back(std::stof(body.substr(p, e - p)));
                        p = e;
                    } else {
                        break;
                    }
                }
                if (out.size() == buckets) return out;
            }
        }
        auto peaks = compute_media_peaks(path, buckets);
        if (peaks.size() == buckets) {
            std::ofstream out(std::filesystem::path(cache),
                              std::ios::binary | std::ios::trunc);
            if (out) {
                out << '[';
                for (size_t i = 0; i < peaks.size(); ++i) {
                    if (i) out << ',';
                    char b[32]{};
                    std::snprintf(b, sizeof(b), "%.4f", static_cast<double>(peaks[i]));
                    out << b;
                }
                out << ']';
            }
        }
        return peaks;
    } catch (...) {
        return {};
    }
}

}  // namespace cliplite::library
