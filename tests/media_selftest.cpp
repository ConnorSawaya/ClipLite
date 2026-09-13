// Media pipeline self-test: creates a D3D11 device and encodes a short
// synthetic video+audio stream to a valid MP4, then verifies the container.
// Console app run by CTest.

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "cliplite/encoder/media_foundation.h"
#include "cliplite/graphics/d3d11_device.h"
#include "cliplite/graphics/frame_converter.h"
#include "cliplite/log.h"

namespace {

void fill_nv12(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, uint32_t frame) {
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t luma = static_cast<uint8_t>((y + frame * 8) % 256);
        std::memset(buf.data() + static_cast<size_t>(y) * w, luma, w);
    }
    std::memset(buf.data() + static_cast<size_t>(w) * h, 128, static_cast<size_t>(w) * h / 2);
}

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cliplite::log::init("cliplite_media_test.log");
    int failures = 0;

    // 0. NV12 downscaler (pure CPU, no device needed).
    {
        // Constant frame scales to itself exactly.
        std::vector<uint8_t> src(static_cast<size_t>(8) * 8 * 3 / 2, 0);
        std::fill(src.begin(), src.begin() + 64, static_cast<uint8_t>(200));
        std::fill(src.begin() + 64, src.end(), static_cast<uint8_t>(128));
        std::vector<uint8_t> dst(static_cast<size_t>(4) * 4 * 3 / 2, 0);
        cliplite::graphics::nv12_scale_down(src.data(), 8, 8, dst.data(), 4, 4);
        bool flat = true;
        for (size_t i = 0; i < 16; ++i) flat = flat && dst[i] == 200;
        for (size_t i = 16; i < 24; ++i) flat = flat && dst[i] == 128;
        if (!flat) {
            std::printf("FAIL nv12 constant downscale\n");
            ++failures;
        } else {
            std::printf("PASS nv12 constant downscale\n");
        }
        // Linear ramp midpoint lands mid-scale (bilinear, not nearest).
        std::vector<uint8_t> ramp(static_cast<size_t>(8) * 8 * 3 / 2, 128);
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t v = static_cast<uint8_t>(x * 32);
            for (uint32_t y = 0; y < 8; ++y) ramp[static_cast<size_t>(y) * 8 + x] = v;
        }
        std::vector<uint8_t> tiny(static_cast<size_t>(4) * 4 * 3 / 2, 0);
        cliplite::graphics::nv12_scale_down(ramp.data(), 8, 8, tiny.data(), 4, 4);
        // dst x=1 samples source ~2.5 -> between 64 and 96.
        const uint8_t mid = tiny[1];
        if (mid < 48 || mid > 112) {
            std::printf("FAIL nv12 ramp downscale (mid=%u)\n", mid);
            ++failures;
        } else {
            std::printf("PASS nv12 ramp downscale (mid=%u)\n", mid);
        }
        // Refusals never crash: odd dims and nulls are no-ops.
        cliplite::graphics::nv12_scale_down(src.data(), 7, 8, dst.data(), 4, 4);
        cliplite::graphics::nv12_scale_down(nullptr, 8, 8, dst.data(), 4, 4);
        cliplite::graphics::nv12_scale_down(src.data(), 8, 8, nullptr, 4, 4);
        std::printf("PASS nv12 refusal paths\n");
    }

    // 1. D3D11 device.
    cliplite::graphics::D3D11Device d3d;
    if (!d3d.create()) {
        std::printf("FAIL d3d11 device\n");
        ++failures;
    } else {
        std::printf("PASS d3d11 device (feature level 0x%x)\n",
                    static_cast<unsigned>(d3d.feature_level()));
    }

    // 2. Encoder: 320x240 @ 30fps for 1 second with a 440 Hz tone.
    const char* out = "cliplite_encoder_test.mp4";
    std::remove(out);

    cliplite::encoder::MediaFoundationEncoder enc;
    cliplite::encoder::EncoderConfig cfg;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps_num = 30;
    cfg.fps_den = 1;
    cfg.video_bitrate_bps = 500'000;
    cfg.audio_enabled = true;

    std::wstring wpath;
    for (char c : std::string(out)) wpath.push_back(static_cast<wchar_t>(c));

    if (!enc.start(wpath, cfg)) {
        std::printf("FAIL encoder start\n");
        ++failures;
    } else {
        const uint32_t w = cfg.width;
        const uint32_t h = cfg.height;
        std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
        const int64_t frame_dur = 10'000'000LL / 30;
        const uint32_t frames = 30;
        const uint32_t audio_frames = cfg.sample_rate / 30;
        std::vector<float> pcm(static_cast<size_t>(audio_frames) * cfg.channels);

        for (uint32_t f = 0; f < frames; ++f) {
            fill_nv12(nv12, w, h, f);
            if (!enc.push_video_frame(nv12.data(), w, static_cast<int64_t>(f) * frame_dur)) {
                std::printf("FAIL video frame %u\n", f);
                ++failures;
                break;
            }
            for (uint32_t i = 0; i < audio_frames; ++i) {
                const float s = 0.05f *
                                std::sinf(6.2831853f * 440.0f *
                                          static_cast<float>(f * audio_frames + i) /
                                          cfg.sample_rate);
                pcm[static_cast<size_t>(i) * 2] = s;
                pcm[static_cast<size_t>(i) * 2 + 1] = s;
            }
            if (!enc.push_audio_frames(pcm.data(), audio_frames,
                                       static_cast<int64_t>(f) * frame_dur)) {
                std::printf("FAIL audio frame %u\n", f);
                ++failures;
                break;
            }
        }
        if (!enc.finish()) {
            std::printf("FAIL encoder finish\n");
            ++failures;
        }
    }

    // 3. Verify MP4 container.
    {
        std::ifstream in(out, std::ios::binary | std::ios::ate);
        if (!in) {
            std::printf("FAIL mp4 file not created\n");
            ++failures;
        } else {
            const std::streamoff size = in.tellg();
            in.seekg(0);
            char hdr[12] = {0};
            in.read(hdr, sizeof(hdr));
            const bool ftyp = std::memcmp(hdr + 4, "ftyp", 4) == 0;
            std::printf("mp4 size=%lld ftyp=%s\n", static_cast<long long>(size),
                        ftyp ? "yes" : "no");
            if (size < 1000 || !ftyp) {
                std::printf("FAIL mp4 not valid\n");
                ++failures;
            } else {
                std::printf("PASS encoder wrote valid mp4\n");
            }
        }
        in.close();
    }

    // The finalized file must be unlocked so the clip writer can move/rename it.
    {
        bool removed = false;
        for (int attempt = 0; attempt < 10 && !removed; ++attempt) {
            removed = (std::remove(out) == 0);
            if (!removed) Sleep(100);
        }
        if (!removed) {
            std::printf("FAIL mp4 still locked/unremovable\n");
            ++failures;
        }
    }

    std::printf(failures == 0 ? "MEDIA SELFTEST PASS\n" : "MEDIA SELFTEST FAIL (%d failures)\n",
                failures);
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
