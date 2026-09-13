// Clip library self-test: directory scan (game derivation + sort) and duration
// probing against a real encoder-produced MP4.

#include <windows.h>
#include <objbase.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cliplite/encoder/media_foundation.h"
#include "cliplite/library/clip_library.h"
#include "cliplite/library/export_clip.h"
#include "cliplite/library/thumbnails.h"
#include "cliplite/library/video_edit.h"
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
    return std::filesystem::temp_directory_path().wstring() + L"\\cliplite_lib_test";
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
    cliplite::log::init("cliplite_library_test.log");

    const std::wstring dir = temp_dir();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);  // clear stale files from crashed runs
    std::filesystem::create_directories(dir, ec);

    std::printf("scan + game derivation\n");
    {
        const wchar_t* names[] = {L"Minecraft_2026-08-21_22-00-00.mp4",
                                  L"Valorant_2026-08-20_10-00-00.mp4",
                                  L"Desktop_2026-08-19_09-00-00.mp4"};
        for (const wchar_t* n : names) {
            std::ofstream f(dir + L"\\" + n, std::ios::binary);
            f << "dummy";
        }

        cliplite::library::ClipLibrary lib;
        lib.scan(dir);
        check(lib.size() == 3, "scanned 3 clips");
        check(lib.clips()[0].game == "Minecraft", "newest first = Minecraft");
        check(lib.clips()[1].game == "Valorant", "second = Valorant");
        check(lib.clips()[2].game == "Desktop", "third = Desktop");
    }

    std::printf("duration probing\n");
    {
        const std::wstring out = dir + L"\\probe_test.mp4";
        cliplite::encoder::MediaFoundationEncoder enc;
        cliplite::encoder::EncoderConfig cfg;
        cfg.width = 320;
        cfg.height = 240;
        cfg.fps_num = 30;
        cfg.fps_den = 1;
        cfg.video_bitrate_bps = 500'000;
        cfg.audio_enabled = false;
        if (!enc.start(out, cfg)) {
            check(false, "encoder start");
        } else {
            const uint32_t w = cfg.width, h = cfg.height;
            std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
            const int64_t frame_dur = 10'000'000LL / 30;
            for (uint32_t f = 0; f < 30; ++f) {
                fill_nv12(nv12, w, h, f);
                enc.push_video_frame(nv12.data(), w, static_cast<int64_t>(f) * frame_dur);
            }
            check(enc.finish(), "encoder finish");

            const int64_t ms = cliplite::library::ClipLibrary::probe_duration_ms(out);
            std::printf("  probed duration = %lld ms\n", static_cast<long long>(ms));
            check(ms >= 900 && ms <= 1500, "probed ~1000ms");

            HBITMAP thumb = cliplite::library::generate_thumbnail(out, 96, 54);
            if (thumb) {
                BITMAP bm{};
                GetObjectW(thumb, sizeof(bm), &bm);
                std::printf("  thumbnail %dx%d\n", bm.bmWidth, bm.bmHeight);
                check(bm.bmWidth == 96 && bm.bmHeight == 54, "thumbnail 96x54");
                DeleteObject(thumb);
            } else {
                check(false, "thumbnail generated");
            }
        }
    }

    std::printf("trim export\n");
    {
        const std::wstring src = dir + L"\\probe_test.mp4";
        const std::wstring dst = dir + L"\\probe_test_trimmed.mp4";
        if (cliplite::library::export_trimmed(src, dst, 200, 700)) {
            check(true, "export_trimmed succeeded");
            const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(dst);
            std::printf("  trimmed duration = %lld ms\n", static_cast<long long>(d));
            check(d >= 300 && d <= 800, "trimmed ~500ms");
            HBITMAP t = cliplite::library::generate_thumbnail(dst, 64, 36);
            if (t) {
                check(true, "trimmed clip decodes");
                DeleteObject(t);
            } else {
                check(false, "trimmed clip decodes");
            }
        } else {
            check(false, "export_trimmed succeeded");
        }
    }

    std::printf("video edit (crop + blur + range)\n");
    {
        const std::wstring src = dir + L"\\probe_test.mp4";
        const std::wstring dst = dir + L"\\probe_test_edited.mp4";
        cliplite::library::EditOptions eo;
        eo.start_ms = 100;
        eo.end_ms = 600;
        eo.crop_x = 80;    // 320x240 -> center 160x120
        eo.crop_y = 60;
        eo.crop_w = 160;
        eo.crop_h = 120;
        cliplite::library::EditBlur bl;
        bl.x = 0.25f;
        bl.y = 0.25f;
        bl.w = 0.5f;
        bl.h = 0.5f;
        eo.blurs.push_back(bl); // BLURON

        if (cliplite::library::edit_video(src, dst, eo, nullptr)) {
            check(true, "edit_video succeeded");
            const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(dst);
            std::printf("  edited duration = %lld ms\n", static_cast<long long>(d));
            check(d >= 350 && d <= 800, "edited ~500ms");
            HBITMAP t = cliplite::library::generate_thumbnail(dst, 64, 36);
            if (t) {
                BITMAP bm{};
                GetObjectW(t, sizeof(bm), &bm);
                check(bm.bmWidth == 64 && bm.bmHeight == 36, "edited clip decodes");
                DeleteObject(t);
            } else {
                check(false, "edited clip decodes");
            }
        } else {
            check(false, "edit_video succeeded");
        }
    }

    std::filesystem::remove_all(dir, ec);

    std::printf(g_failures == 0 ? "LIBRARY SELFTEST PASS\n" : "LIBRARY SELFTEST FAIL (%d)\n",
                g_failures);
    cliplite::log::shutdown();
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
