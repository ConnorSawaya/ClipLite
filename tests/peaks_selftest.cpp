// Peaks + filmstrip self-test: streaming audio peaks over a WAV (no codec in
// the loop) and seek-thumbnails over an encoder MP4. Deterministic on any box.

#include <windows.h>
#include <objbase.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "cliplite/audio/stem_writer.h"
#include "cliplite/encoder/media_foundation.h"
#include "cliplite/library/audio_peaks.h"
#include "cliplite/library/thumbnails.h"
#include "cliplite/log.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

std::wstring temp_dir() {
    return std::filesystem::temp_directory_path().wstring() + L"\\cliplite_peaks_test";
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

void fill_nv12(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, uint32_t frame) {
    for (uint32_t y = 0; y < h; ++y) {
        std::memset(buf.data() + static_cast<size_t>(y) * w,
                    static_cast<uint8_t>((y + frame * 8) % 256), w);
    }
    std::memset(buf.data() + static_cast<size_t>(w) * h, 128, static_cast<size_t>(w) * h / 2);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cliplite::log::init("cliplite_peaks_test.log");

    const std::wstring dir = temp_dir();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    std::printf("media peaks over wav\n");
    {
        // First half silence, second half 0.5-amplitude sine.
        std::vector<float> pcm(static_cast<size_t>(48000) * 2, 0.f);
        for (uint32_t f = 24000; f < 48000; ++f) {
            const float v =
                0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / 48000.0f);
            pcm[static_cast<size_t>(f) * 2] = v;
            pcm[static_cast<size_t>(f) * 2 + 1] = v;
        }
        cliplite::audio::StemSegmentWriter w;
        const std::wstring wav = dir + L"\\tone.wav";
        check(w.open(wide_to_utf8(wav)), "stem open");
        check(w.append(pcm.data(), 48000), "stem append");
        check(w.close(), "stem close");

        const auto peaks = cliplite::library::compute_media_peaks(wav, 10);
        check(peaks.size() == 10, "10 buckets");
        bool shape = peaks.size() == 10;
        for (int i = 0; i < 5 && shape; ++i) shape = peaks[static_cast<size_t>(i)] == 0.f;
        for (int i = 5; i < 10 && shape; ++i) shape = peaks[static_cast<size_t>(i)] > 0.45f;
        check(shape, "silence then tone");
        // Cached wrapper agrees (and warms the disk cache).
        const auto cached = cliplite::library::cached_media_peaks(wav, 10);
        check(cached == peaks, "cache matches compute");
        const auto cached2 = cliplite::library::cached_media_peaks(wav, 10);
        check(cached2 == peaks, "cache hit stable");
        check(cliplite::library::compute_media_peaks(wav, 0).empty(), "zero buckets empty");
        check(cliplite::library::compute_media_peaks(dir + L"\\missing.wav", 10).empty(),
              "missing file empty");
    }

    std::printf("filmstrip\n");
    {
        const std::wstring clip = dir + L"\\strip.mp4";
        cliplite::encoder::MediaFoundationEncoder enc;
        cliplite::encoder::EncoderConfig cfg;
        cfg.width = 320;
        cfg.height = 240;
        cfg.fps_num = 30;
        cfg.fps_den = 1;
        cfg.video_bitrate_bps = 500'000;
        cfg.audio_enabled = false;
        if (!enc.start(clip, cfg)) {
            check(false, "strip encoder start");
        } else {
            std::vector<uint8_t> nv12(static_cast<size_t>(320) * 240 * 3 / 2);
            const int64_t frame_dur = 10'000'000LL / 30;
            for (uint32_t f = 0; f < 30; ++f) {
                fill_nv12(nv12, 320, 240, f);
                enc.push_video_frame(nv12.data(), 320, static_cast<int64_t>(f) * frame_dur);
            }
            check(enc.finish(), "strip encoder finish");
            for (int64_t t : {0, 500, 999}) {
                HBITMAP bmp = cliplite::library::generate_thumbnail_at(clip, 96, 54, t);
                if (bmp) {
                    BITMAP bm{};
                    GetObjectW(bmp, sizeof(bm), &bm);
                    check(bm.bmWidth == 96 && bm.bmHeight == 54, "strip thumb decodes");
                    DeleteObject(bmp);
                } else {
                    check(false, "strip thumb generated");
                }
                check(!cliplite::library::cached_thumbnail_file_at(clip, 96, 54, t).empty(),
                      "strip thumb cached");
            }
        }
    }

    std::filesystem::remove_all(dir, ec);

    std::printf(g_failures == 0 ? "PEAKS SELFTEST PASS\n" : "PEAKS SELFTEST FAIL (%d)\n",
                g_failures);
    cliplite::log::shutdown();
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
