// Video player self-test: encodes a short clip, plays it through the MF media
// session (EVR + SAR), and verifies playback starts, advances, and reaches the end.

#include <windows.h>
#include <objbase.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "cliplite/encoder/media_foundation.h"
#include "cliplite/log.h"
#include "cliplite/player/video_player.h"

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

void fill_nv12(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, uint32_t frame) {
    for (uint32_t y = 0; y < h; ++y) {
        std::memset(buf.data() + static_cast<size_t>(y) * w,
                    static_cast<uint8_t>((y + frame * 8) % 256), w);
    }
    std::memset(buf.data() + static_cast<size_t>(w) * h, 128, static_cast<size_t>(w) * h / 2);
}

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cliplite::log::init("cliplite_player_test.log");

    // Register a window class for the (hidden) render window.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClipLitePlayerTest";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::printf("FAIL create window\n");
        return 1;
    }

    // 1. Encode a ~1s clip with audio.
    const std::wstring clip = std::filesystem::temp_directory_path().wstring() + L"\\cliplite_play_test.mp4";
    std::error_code ec;
    std::filesystem::remove(clip, ec);

    cliplite::encoder::MediaFoundationEncoder enc;
    cliplite::encoder::EncoderConfig cfg;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps_num = 30;
    cfg.fps_den = 1;
    cfg.video_bitrate_bps = 500'000;
    cfg.audio_enabled = true;

    if (!enc.start(clip, cfg)) {
        std::printf("FAIL encoder start\n");
        ++g_failures;
    } else {
        const uint32_t w = cfg.width, h = cfg.height;
        std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
        const int64_t frame_dur = 10'000'000LL / 30;
        const uint32_t frames = 30;
        const uint32_t af = cfg.sample_rate / 30;
        std::vector<float> pcm(static_cast<size_t>(af) * cfg.channels);
        for (uint32_t f = 0; f < frames; ++f) {
            fill_nv12(nv12, w, h, f);
            enc.push_video_frame(nv12.data(), w, static_cast<int64_t>(f) * frame_dur);
            for (uint32_t i = 0; i < af; ++i) {
                const float s = 0.05f * std::sinf(6.2831853f * 440.0f *
                                                  static_cast<float>(f * af + i) / cfg.sample_rate);
                pcm[static_cast<size_t>(i) * 2] = s;
                pcm[static_cast<size_t>(i) * 2 + 1] = s;
            }
            enc.push_audio_frames(pcm.data(), af, static_cast<int64_t>(f) * frame_dur);
        }
        if (!enc.finish()) {
            std::printf("FAIL encoder finish\n");
            ++g_failures;
        }
    }

    // 2. Play it back.
    cliplite::player::VideoPlayer player;
    if (!player.open(hwnd, clip)) {
        std::printf("FAIL player open\n");
        ++g_failures;
    } else {
        std::printf("opened clip, duration=%lld ms\n", static_cast<long long>(player.duration_ms()));
        int64_t max_pos = 0;
        bool saw_playing = false;
        const ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 5000 && !player.ended()) {
            player.process_events();
            const int64_t pos = player.position_ms();
            if (pos > max_pos) max_pos = pos;
            if (player.playing()) saw_playing = true;
            Sleep(20);
        }
        player.process_events();
        std::printf("max_pos=%lld ms, saw_playing=%d, ended=%d\n",
                    static_cast<long long>(max_pos), saw_playing ? 1 : 0, player.ended() ? 1 : 0);
        check(saw_playing || player.ended(), "playback started");
        check(max_pos >= 300 || player.ended(), "position advanced");
        check(player.ended(), "reached end of clip");
        player.close();
    }

    DestroyWindow(hwnd);
    std::filesystem::remove(clip, ec);

    std::printf(g_failures == 0 ? "PLAYER SELFTEST PASS\n" : "PLAYER SELFTEST FAIL (%d)\n",
                g_failures);
    cliplite::log::shutdown();
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
