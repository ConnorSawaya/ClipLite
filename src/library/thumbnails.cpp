#include "cliplite/library/thumbnails.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "cliplite/log.h"

namespace cliplite::library {

namespace {

HBITMAP scale_bitmap(HBITMAP src, int tw, int th) {
    BITMAP bm{};
    GetObjectW(src, sizeof(bm), &bm);

    HDC src_dc = CreateCompatibleDC(nullptr);
    SelectObject(src_dc, src);
    HDC dst_dc = CreateCompatibleDC(nullptr);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -th;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dst = CreateDIBSection(dst_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dst) {
        SelectObject(dst_dc, dst);
        SetStretchBltMode(dst_dc, HALFTONE);
        StretchBlt(dst_dc, 0, 0, tw, th, src_dc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    }
    DeleteDC(src_dc);
    DeleteDC(dst_dc);
    return dst;
}

inline uint8_t clamp255(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// BT.601 limited-range NV12 -> BGRA (matches frame_converter's forward path).
void nv12_to_bgra(const uint8_t* nv12, int w, int h, uint8_t* bgra) {
    const uint8_t* y = nv12;
    const uint8_t* uv = nv12 + static_cast<size_t>(w) * h;
    for (int j = 0; j < h; ++j) {
        const uint8_t* uvrow = uv + static_cast<size_t>(j / 2) * w;
        uint8_t* out = bgra + static_cast<size_t>(j) * w * 4;
        for (int i = 0; i < w; ++i) {
            const int yy = y[static_cast<size_t>(j) * w + i];
            const int u = uvrow[(i & ~1)];
            const int v = uvrow[(i & ~1) + 1];
            const int c = yy - 16;
            const int d = u - 128;
            const int e = v - 128;
            out[i * 4 + 0] = clamp255((298 * c + 516 * d + 128) >> 8);  // B
            out[i * 4 + 1] = clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);  // G
            out[i * 4 + 2] = clamp255((298 * c + 409 * e + 128) >> 8);  // R
            out[i * 4 + 3] = 255;
        }
    }
}

std::wstring known_local_app_data() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        std::wstring dir(raw);
        CoTaskMemFree(raw);
        return dir;
    }
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
}

std::filesystem::path thumb_cache_dir() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(known_local_app_data()) / L"ClipLite" / L"thumbs";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

FILETIME source_write_time(const std::wstring& path) {
    FILETIME ft{};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        GetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
    }
    return ft;
}

std::filesystem::path cache_file_for(const std::wstring& path, int width, int height) {
    const FILETIME ft = source_write_time(path);
    const std::wstring key =
        path + L"|" + std::to_wstring(static_cast<uint64_t>(ft.dwHighDateTime) << 32 |
                                      ft.dwLowDateTime);
    const size_t hash_value = std::hash<std::wstring>{}(key);
    wchar_t name[96]{};
    swprintf_s(name, L"%016llX_%d_%d.bmp", static_cast<unsigned long long>(hash_value), width,
               height);
    return thumb_cache_dir() / name;
}

std::filesystem::path cache_file_for_at(const std::wstring& path, int width, int height,
                                        int64_t time_ms) {
    const FILETIME ft = source_write_time(path);
    const std::wstring key =
        path + L"|" + std::to_wstring(static_cast<uint64_t>(ft.dwHighDateTime) << 32 |
                                      ft.dwLowDateTime);
    const size_t hash_value = std::hash<std::wstring>{}(key);
    wchar_t name[96]{};
    swprintf_s(name, L"%016llX_%d_%d_%lld.bmp", static_cast<unsigned long long>(hash_value),
               width, height, static_cast<long long>(time_ms < 0 ? 0 : time_ms));
    return thumb_cache_dir() / name;
}

HBITMAP load_cached_bitmap(const std::filesystem::path& file) {
    if (file.empty() || !std::filesystem::exists(file)) return nullptr;
    return static_cast<HBITMAP>(LoadImageW(nullptr, file.c_str(), IMAGE_BITMAP, 0, 0,
                                           LR_LOADFROMFILE | LR_CREATEDIBSECTION));
}

void save_bitmap_to_cache(const std::filesystem::path& file, HBITMAP bmp) {
    BITMAP bm{};
    if (file.empty() || !bmp || !GetObjectW(bmp, sizeof(bm), &bm)) return;
    const LONG w = bm.bmWidth;
    const LONG h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, bmp, 0, static_cast<UINT>(h), pixels.data(), &bmi,
                                DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines != h) return;

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;  // "BM"
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = static_cast<DWORD>(fh.bfOffBits + pixels.size());

    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char*>(&bmi.bmiHeader), sizeof(BITMAPINFOHEADER));
    f.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
}

}  // namespace

std::wstring thumbnail_cache_dir() {
    try {
        return thumb_cache_dir().wstring();
    } catch (...) {
        return {};
    }
}

std::wstring cached_thumbnail_file(const std::wstring& path, int width, int height) {
    try {
        const std::filesystem::path file = cache_file_for(path, width, height);
        std::error_code ec;
        if (file.empty() || !std::filesystem::exists(file, ec)) return {};
        // Reject truncated/corrupt entries: a valid BMP carries both headers.
        if (std::filesystem::file_size(file, ec) <=
            sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))
            return {};
        return file.wstring();
    } catch (...) {
        return {};
    }
}

HBITMAP generate_thumbnail_impl(const std::wstring& path, int width, int height,
                                int64_t time_ms, const std::filesystem::path& cache_path) {
    try {
        HBITMAP cached = load_cached_bitmap(cache_path);
        if (cached) return cached;
    } catch (...) {
    }

    const HRESULT startup = MFStartup(MF_VERSION);
    HBITMAP result = nullptr;

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (SUCCEEDED(hr)) {
        UINT32 nw = 0;
        UINT32 nh = 0;
        Microsoft::WRL::ComPtr<IMFMediaType> native;
        if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
            MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
        }

        Microsoft::WRL::ComPtr<IMFMediaType> type;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (nw > 0 && nh > 0) MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, nw, nh);
        MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get());

        if (time_ms >= 0) {
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = time_ms * 10'000;
            reader->SetCurrentPosition(GUID_NULL, pos);
            PropVariantClear(&pos);
        }

        DWORD flags = 0;
        DWORD stream = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts,
                                &sample);
        if (SUCCEEDED(hr) && sample) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                BYTE* data = nullptr;
                DWORD max_len = 0;
                DWORD cur_len = 0;
                if (SUCCEEDED(buffer->Lock(&data, &max_len, &cur_len)) && data) {
                    UINT32 w = 0;
                    UINT32 h = 0;
                    Microsoft::WRL::ComPtr<IMFMediaType> cur;
                    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
                    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
                    if (w > 0 && h > 0 && cur_len >= static_cast<DWORD>(w) * h * 3 / 2) {
                        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
                        nv12_to_bgra(data, static_cast<int>(w), static_cast<int>(h), bgra.data());

                        BITMAPINFO bmi{};
                        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth = static_cast<LONG>(w);
                        bmi.bmiHeader.biHeight = -static_cast<LONG>(h);  // top-down BGRA
                        bmi.bmiHeader.biPlanes = 1;
                        bmi.bmiHeader.biBitCount = 32;
                        bmi.bmiHeader.biCompression = BI_RGB;
                        void* bits = nullptr;
                        HBITMAP full = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits,
                                                        nullptr, 0);
                        if (full && bits) {
                            std::memcpy(bits, bgra.data(), bgra.size());
                            result = scale_bitmap(full, width, height);
                            DeleteObject(full);
                        }
                    }
                    buffer->Unlock();
                }
            }
        }
    }
    reader.Reset();
    if (SUCCEEDED(startup)) MFShutdown();

    if (result) {
        try {
            save_bitmap_to_cache(cache_path, result);
        } catch (...) {
        }
    }
    return result;
}

HBITMAP generate_thumbnail(const std::wstring& path, int width, int height) {
    try {
        return generate_thumbnail_impl(path, width, height, -1,
                                       cache_file_for(path, width, height));
    } catch (...) {
        return nullptr;
    }
}

HBITMAP generate_thumbnail_at(const std::wstring& path, int width, int height,
                              int64_t time_ms) {
    try {
        if (time_ms < 0) time_ms = 0;
        return generate_thumbnail_impl(path, width, height, time_ms,
                                       cache_file_for_at(path, width, height, time_ms));
    } catch (...) {
        return nullptr;
    }
}

std::wstring cached_thumbnail_file_at(const std::wstring& path, int width, int height,
                                      int64_t time_ms) {
    try {
        if (time_ms < 0) time_ms = 0;
        const std::filesystem::path file = cache_file_for_at(path, width, height, time_ms);
        std::error_code ec;
        if (file.empty() || !std::filesystem::exists(file, ec)) return {};
        if (std::filesystem::file_size(file, ec) <=
            sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))
            return {};
        return file.wstring();
    } catch (...) {
        return {};
    }
}

}  // namespace cliplite::library
