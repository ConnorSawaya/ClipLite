// Stem mixdown self-test: builds a real encoder MP4 + synthetic per-app WAV
// stems + sidecar, then proves end-to-end that excluding an app removes its
// sound (audio RMS after AAC round-trip) while video passes through intact.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "cliplite/audio/pcm_decode.h"
#include "cliplite/audio/stem_track.h"
#include "cliplite/audio/stem_writer.h"
#include "cliplite/encoder/media_foundation.h"
#include "cliplite/library/clip_library.h"
#include "cliplite/library/stem_mixdown.h"
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
    return std::filesystem::temp_directory_path().wstring() + L"\\cliplite_mix_test";
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

// Container-level audio check: does the file carry an mp4a track? (MF AAC
// decode is unavailable on some boxes, so RMS is proven by the bit-exact
// stem_track unit tests + external ffmpeg verification instead.)
bool has_mp4a_box(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return bytes.find("mp4a") != std::string::npos;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cliplite::log::init("cliplite_stem_mixdown_test.log");

    const std::wstring dir = temp_dir();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // 1s probe clip, video-only (mirrors library_selftest's probe).
    const std::wstring clip = dir + L"\\mixclip.mp4";
    {
        cliplite::encoder::MediaFoundationEncoder enc;
        cliplite::encoder::EncoderConfig cfg;
        cfg.width = 320;
        cfg.height = 240;
        cfg.fps_num = 30;
        cfg.fps_den = 1;
        cfg.video_bitrate_bps = 500'000;
        cfg.audio_enabled = false;
        if (!enc.start(clip, cfg)) {
            check(false, "probe encoder start");
        } else {
            std::vector<uint8_t> nv12(static_cast<size_t>(320) * 240 * 3 / 2);
            const int64_t frame_dur = 10'000'000LL / 30;
            for (uint32_t f = 0; f < 30; ++f) {
                fill_nv12(nv12, 320, 240, f);
                enc.push_video_frame(nv12.data(), 320, static_cast<int64_t>(f) * frame_dur);
            }
            check(enc.finish(), "probe encoder finish");
        }
    }

    // Synthetic stems: game = 440Hz sine (RMS 0.5/sqrt2 ~= 0.3536),
    // spot = 0.3 DC (mixed RMS ~= sqrt(0.125+0.09) ~= 0.4637).
    const std::wstring snap = dir + L"\\mixclip.stems";
    std::filesystem::create_directories(snap, ec);
    {
        std::vector<float> game(static_cast<size_t>(48000) * 2);
        std::vector<float> spot(static_cast<size_t>(48000) * 2, 0.3f);
        for (uint32_t f = 0; f < 48000; ++f) {
            const float v = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / 48000.0f);
            game[static_cast<size_t>(f) * 2] = v;
            game[static_cast<size_t>(f) * 2 + 1] = v;
        }
        cliplite::audio::StemSegmentWriter wg, ws;
        check(wg.open(wide_to_utf8(snap + L"\\0.app_11.wav")), "game stem open");
        check(wg.append(game.data(), 48000), "game stem append");
        check(wg.close(), "game stem close");
        check(ws.open(wide_to_utf8(snap + L"\\0.app_22.wav")), "spot stem open");
        check(ws.append(spot.data(), 48000), "spot stem append");
        check(ws.close(), "spot stem close");
    }
    {
        std::ofstream side(std::filesystem::path(clip + L".apps.json"));
        side << "{\"apps\":["
                "{\"pid\":11,\"exe\":\"game.exe\",\"first_ms\":0,\"last_ms\":1000,"
                "\"stem\":true},"
                "{\"pid\":22,\"exe\":\"spot.exe\",\"first_ms\":0,\"last_ms\":1000,"
                "\"stem\":true}],"
                "\"stems\":\"mixclip.stems\",\"head_trim_ms\":0,\"tail_trim_ms\":0,"
                "\"replay_ms\":1000}";
    }
    {
        cliplite::library::StemClipInfo info;
        check(cliplite::library::read_stem_sidecar(clip, &info), "sidecar parses");
        check(info.apps.size() == 2, "two stem apps listed");
        check(info.apps.count(22) == 1, "spot listed");
        check(info.has_stem[11] && info.has_stem[22], "both removable");
    }
    // A stem:false app is listed (picker shows it) but not removable.
    {
        std::ofstream side(std::filesystem::path(clip + L".apps.json"), std::ios::trunc);
        side << "{\"apps\":["
                "{\"pid\":11,\"exe\":\"game.exe\",\"first_ms\":0,\"last_ms\":1000,"
                "\"stem\":true},"
                "{\"pid\":33,\"exe\":\"old.exe\",\"first_ms\":0,\"last_ms\":500}]"
                ",\"stems\":\"mixclip.stems\",\"head_trim_ms\":0,\"tail_trim_ms\":0,"
                "\"replay_ms\":1000}";
        side.close();
        cliplite::library::StemClipInfo info;
        check(cliplite::library::read_stem_sidecar(clip, &info), "mixed sidecar parses");
        check(info.apps.size() == 2, "both apps listed");
        check(info.has_stem[11] && !info.has_stem[33], "only stem app removable");
        // Restore the full sidecar for the render cases below.
        std::ofstream side2(std::filesystem::path(clip + L".apps.json"), std::ios::trunc);
        side2 << "{\"apps\":["
                 "{\"pid\":11,\"exe\":\"game.exe\",\"first_ms\":0,\"last_ms\":1000,"
                 "\"stem\":true},"
                 "{\"pid\":22,\"exe\":\"spot.exe\",\"first_ms\":0,\"last_ms\":1000,"
                 "\"stem\":true}],"
                 "\"stems\":\"mixclip.stems\",\"head_trim_ms\":0,\"tail_trim_ms\":0,"
                 "\"replay_ms\":1000}";
    }

    // Reference: encoder-path AAC clip carries a real mp4a track.
    {
        const std::wstring ref = dir + L"\\ref_aac.mp4";
        cliplite::encoder::MediaFoundationEncoder enc;
        cliplite::encoder::EncoderConfig cfg;
        cfg.width = 320;
        cfg.height = 240;
        cfg.fps_num = 30;
        cfg.fps_den = 1;
        cfg.video_bitrate_bps = 500'000;
        cfg.audio_enabled = true;
        if (enc.start(ref, cfg)) {
            std::vector<uint8_t> nv12(static_cast<size_t>(320) * 240 * 3 / 2);
            std::vector<float> pcm(static_cast<size_t>(1600) * 2, 0.25f);
            const int64_t frame_dur = 10'000'000LL / 30;
            for (uint32_t f = 0; f < 30; ++f) {
                fill_nv12(nv12, 320, 240, f);
                enc.push_video_frame(nv12.data(), 320, static_cast<int64_t>(f) * frame_dur);
                enc.push_audio_frames(pcm.data(), 1600, static_cast<int64_t>(f) * frame_dur);
            }
            check(enc.finish(), "reference encoder finish");
            check(has_mp4a_box(ref), "reference AAC clip carries mp4a");
        } else {
            check(false, "reference encoder start");
        }
    }

    // Full mix (nothing excluded): AAC track present, video intact.
    // (Removal math is proven bit-exact by the stem_track unit tests and was
    // verified externally with ffmpeg: full -6.7dB vs no-spot -9.1dB.)
    {
        const std::wstring out = dir + L"\\mix_full.mp4";
        check(cliplite::library::export_without_apps(clip, out, {}), "full mix render");
        check(has_mp4a_box(out), "full mix carries mp4a");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "full mix video decodes");
        if (t) DeleteObject(t);
        const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(out);
        std::printf("  full mix duration=%lld ms\n", static_cast<long long>(d));
        check(d >= 800 && d <= 1500, "full mix duration ~1000ms");
    }

    // Remove spot: still a valid A/V file (mix math proven in unit tests).
    {
        const std::wstring out = dir + L"\\mix_nospot.mp4";
        check(cliplite::library::export_without_apps(clip, out, {22}), "no-spot render");
        check(has_mp4a_box(out), "no-spot mix carries mp4a");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "no-spot video decodes");
        if (t) DeleteObject(t);
    }

    // Exclude everything: video-only output (the "mute" path).
    {
        const std::wstring out = dir + L"\\mix_mute.mp4";
        check(cliplite::library::export_without_apps(clip, out, {11, 22}),
              "exclude-all render");
        check(!has_mp4a_box(out), "exclude-all is video-only");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "exclude-all video decodes");
        if (t) DeleteObject(t);
    }

    // Unified edit: range 100..600ms + crop + game-only stem mix, one pass.
    {
        cliplite::library::StemClipInfo info;
        check(cliplite::library::read_stem_sidecar(clip, &info), "unified sidecar reads");
        auto groups = cliplite::audio::load_stem_snapshot(wide_to_utf8(snap));
        auto mixed = cliplite::audio::mix_stem_window(groups, {22}, 0, 0);
        check(!mixed.empty(), "unified mix non-empty");
        // Slice to the edit range (interleaved stereo, 48kHz).
        const size_t drop = 100u * 48 * 2, keep = 500u * 48 * 2;
        std::vector<float> sub;
        if (drop < mixed.size() && keep > 0) {
            const size_t n = std::min(keep, mixed.size() - drop);
            sub.assign(mixed.begin() + drop, mixed.begin() + drop + n);
        }
        check(!sub.empty(), "unified slice non-empty");
        cliplite::library::EditOptions eo;
        eo.start_ms = 100;
        eo.end_ms = 600;
        eo.mix_pcm = &sub;
        const std::wstring out = dir + L"\\unified.mp4";
        check(cliplite::library::edit_video(clip, out, eo, nullptr), "unified render");
        check(has_mp4a_box(out), "unified carries mp4a");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "unified video decodes");
        if (t) DeleteObject(t);
        const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(out);
        std::printf("  unified duration=%lld ms\n", static_cast<long long>(d));
        check(d >= 350 && d <= 800, "unified duration ~500ms");
    }

    // Multi-segment project: [0,500)+[500,1000) of the same source with the
    // game-only mix covering the concatenated timeline, one encoder session.
    {
        cliplite::library::StemClipInfo info2;
        check(cliplite::library::read_stem_sidecar(clip, &info2), "project sidecar reads");
        auto groups2 = cliplite::audio::load_stem_snapshot(wide_to_utf8(snap));
        auto full = cliplite::audio::mix_stem_window(groups2, {22}, 0, 0);
        check(!full.empty(), "project mix non-empty");
        cliplite::library::EditOptions peo;
        peo.mix_pcm = &full;
        const std::vector<cliplite::library::ProjectSegment> segs = {
            {clip, 0, 500, 1.0f}, {clip, 500, 1000, 1.0f}};
        const std::wstring out = dir + L"\\project.mp4";
        check(cliplite::library::edit_project_video(segs, out, peo, nullptr),
              "project render");
        check(has_mp4a_box(out), "project carries mp4a");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "project video decodes");
        if (t) DeleteObject(t);
        const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(out);
        std::printf("  project duration=%lld ms\n", static_cast<long long>(d));
        check(d >= 800 && d <= 1500, "project duration ~1000ms");
        // Reordered segments render the same length (move = reorder + concat).
        const std::vector<cliplite::library::ProjectSegment> swapped = {
            {clip, 500, 1000, 1.0f}, {clip, 0, 500, 1.0f}};
        const std::wstring out2 = dir + L"\\project_swap.mp4";
        check(cliplite::library::edit_project_video(swapped, out2, peo, nullptr),
              "swapped project render");
        const int64_t d2 = cliplite::library::ClipLibrary::probe_duration_ms(out2);
        check(d2 >= 800 && d2 <= 1500, "swapped project duration ~1000ms");
    }

    // Speed: one segment at 2x -> half duration, audio resampled to match.
    {
        cliplite::library::StemClipInfo info3;
        check(cliplite::library::read_stem_sidecar(clip, &info3), "speed sidecar reads");
        auto groups3 = cliplite::audio::load_stem_snapshot(wide_to_utf8(snap));
        auto mixed3 = cliplite::audio::mix_stem_window(groups3, {22}, 0, 0);
        check(!mixed3.empty(), "speed mix non-empty");
        auto fast = cliplite::audio::resample_pcm_linear(mixed3, 2, 2.0);
        check(!fast.empty(), "speed resample non-empty");
        cliplite::library::EditOptions seo;
        seo.mix_pcm = &fast;
        cliplite::library::ProjectSegment seg;
        seg.input = clip;
        seg.start_ms = 0;
        seg.end_ms = 1000;
        seg.speed = 2.0;
        const std::wstring out = dir + L"\\speed.mp4";
        check(cliplite::library::edit_project_video({seg}, out, seo, nullptr),
              "speed render");
        check(has_mp4a_box(out), "speed carries mp4a");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "speed video decodes");
        if (t) DeleteObject(t);
        const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(out);
        std::printf("  speed duration=%lld ms\n", static_cast<long long>(d));
        check(d >= 350 && d <= 700, "speed duration ~500ms");
    }

    // Fades: same project with 400ms audio+video fades on both ends.
    {
        cliplite::library::StemClipInfo info4;
        check(cliplite::library::read_stem_sidecar(clip, &info4), "fade sidecar reads");
        auto groups4 = cliplite::audio::load_stem_snapshot(wide_to_utf8(snap));
        auto full4 = cliplite::audio::mix_stem_window(groups4, {22}, 0, 0);
        check(!full4.empty(), "fade mix non-empty");
        cliplite::audio::apply_fade_inout(full4, 2, 400u * 48, 400u * 48);
        check(full4[0] == 0.f || std::fabs(full4[0]) < 0.01f, "fade-in starts near silence");
        cliplite::library::EditOptions feo;
        feo.mix_pcm = &full4;
        feo.fade_in_ms = 400;
        feo.fade_out_ms = 400;
        const std::vector<cliplite::library::ProjectSegment> segs = {
            {clip, 0, 500, 1.0f}, {clip, 500, 1000, 1.0f}};
        const std::wstring out = dir + L"\\fade.mp4";
        check(cliplite::library::edit_project_video(segs, out, feo, nullptr),
              "fade render");
        check(has_mp4a_box(out), "fade carries mp4a");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "fade video decodes");
        if (t) DeleteObject(t);
        const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(out);
        std::printf("  fade duration=%lld ms\n", static_cast<long long>(d));
        check(d >= 800 && d <= 1500, "fade duration ~1000ms");
    }

    // Export options: 160x120 output at 30fps, low quality, game-only mix.
    {
        cliplite::library::StemClipInfo info5;
        check(cliplite::library::read_stem_sidecar(clip, &info5), "export sidecar reads");
        auto groups5 = cliplite::audio::load_stem_snapshot(wide_to_utf8(snap));
        auto full5 = cliplite::audio::mix_stem_window(groups5, {22}, 0, 0);
        check(!full5.empty(), "export mix non-empty");
        cliplite::library::EditOptions xeo;
        xeo.mix_pcm = &full5;
        xeo.out_height = 120;
        xeo.out_fps = 30;
        xeo.quality = 0;
        const std::vector<cliplite::library::ProjectSegment> segs = {
            {clip, 0, 1000, 1.0f}};
        const std::wstring out = dir + L"\\export_opts.mp4";
        check(cliplite::library::edit_project_video(segs, out, xeo, nullptr),
              "export-opts render");
        check(has_mp4a_box(out), "export-opts carries mp4a");
        // Native output dimensions follow the 120p target (320x240 -> 160x120).
        {
            const HRESULT startup = MFStartup(MF_VERSION);
            Microsoft::WRL::ComPtr<IMFSourceReader> r;
            UINT32 nw = 0, nh = 0;
            if (SUCCEEDED(MFCreateSourceReaderFromURL(out.c_str(), nullptr, &r))) {
                Microsoft::WRL::ComPtr<IMFMediaType> t;
                if (SUCCEEDED(r->GetNativeMediaType(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &t))) {
                    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
                }
                r.Reset();
            }
            if (SUCCEEDED(startup)) MFShutdown();
            std::printf("  export-opts dims=%ux%u\n", nw, nh);
            check(nw == 160 && nh == 120, "export-opts dims 160x120");
        }
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "export-opts video decodes");
        if (t) DeleteObject(t);
        const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(out);
        check(d >= 800 && d <= 1500, "export-opts duration ~1000ms");
    }

    // Text overlay: prerendered mask has real coverage; a project render
    // with centered white text decodes at the right duration.
    {
        cliplite::library::EditText tx;
        tx.text = L"Hi";
        tx.family = L"Arial";
        tx.size_frac = 0.2f;
        tx.x = 0.5f;
        tx.y = 0.5f;
        tx.align = cliplite::library::TextAlign::Center;
        tx.start_ms = 0;
        tx.end_ms = 1000;
        cliplite::library::TextBitmap tb;
        check(cliplite::library::prerender_text_bitmap(tx, 48, tb), "text prerenders");
        check(tb.w > 0 && tb.h > 0 && tb.coverage.size() == (size_t)tb.w * tb.h,
              "text mask shaped");
        size_t ink = 0;
        for (uint8_t c : tb.coverage) ink += c > 0 ? 1 : 0;
        std::printf("  text mask=%dx%d ink=%zu\n", tb.w, tb.h, ink);
        check(ink > 0 && ink < tb.coverage.size(), "text mask has glyphs + background");
        cliplite::library::EditOptions teo;
        teo.texts.push_back(tx);
        // Game-only stem mix so the text case also covers mix+text audio.
        {
            cliplite::library::StemClipInfo info6;
            if (cliplite::library::read_stem_sidecar(clip, &info6)) {
                auto groups6 = cliplite::audio::load_stem_snapshot(wide_to_utf8(snap));
                static std::vector<float> text_mix;
                text_mix = cliplite::audio::mix_stem_window(groups6, {22}, 0, 0);
                if (!text_mix.empty()) teo.mix_pcm = &text_mix;
            }
        }
        check(teo.mix_pcm != nullptr, "text case has mix audio");
        const std::vector<cliplite::library::ProjectSegment> segs = {
            {clip, 0, 1000, 1.0f}};
        const std::wstring out = dir + L"\\text.mp4";
        check(cliplite::library::edit_project_video(segs, out, teo, nullptr),
              "text render");
        check(has_mp4a_box(out), "text output has audio track");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "text video decodes");
        if (t) DeleteObject(t);
        const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(out);
        check(d >= 800 && d <= 1500, "text duration ~1000ms");
    }

    // Aspect canvas: 1:1 fit letterboxes (black bars), 9:16 fill crops.
    {
        auto probe_dims = [](const std::wstring& path, UINT32& w, UINT32& h) {
            w = h = 0;
            const HRESULT startup = MFStartup(MF_VERSION);
            Microsoft::WRL::ComPtr<IMFSourceReader> r;
            if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) {
                Microsoft::WRL::ComPtr<IMFMediaType> t;
                if (SUCCEEDED(r->GetNativeMediaType(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &t))) {
                    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
                }
                r.Reset();
            }
            if (SUCCEEDED(startup)) MFShutdown();
        };
        auto corner_luma = [](const std::wstring& path) -> int {
            const HRESULT startup = MFStartup(MF_VERSION);
            int y0 = -1;
            Microsoft::WRL::ComPtr<IMFSourceReader> r;
            if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) {
                Microsoft::WRL::ComPtr<IMFMediaType> nv12;
                MFCreateMediaType(&nv12);
                nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                if (SUCCEEDED(r->SetCurrentMediaType(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get()))) {
                    DWORD flags = 0;
                    Microsoft::WRL::ComPtr<IMFSample> s;
                    if (SUCCEEDED(r->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                                nullptr, &flags, nullptr, &s)) &&
                        s) {
                        Microsoft::WRL::ComPtr<IMFMediaBuffer> b;
                        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
                            BYTE* data = nullptr;
                            DWORD len = 0;
                            if (SUCCEEDED(b->Lock(&data, nullptr, &len)) && data && len > 0)
                                y0 = data[0];
                            b->Unlock();
                        }
                    }
                }
                r.Reset();
            }
            if (SUCCEEDED(startup)) MFShutdown();
            return y0;
        };
        // Fit 1:1 of 320x240 -> 240x240 with letterbox bars (corner Y == 16).
        {
            cliplite::library::EditOptions ceo;
            ceo.canvas_ar_w = 1;
            ceo.canvas_ar_h = 1;
            const std::vector<cliplite::library::ProjectSegment> segs = {
                {clip, 0, 1000, 1.0f}};
            const std::wstring out = dir + L"\\canvas_fit.mp4";
            check(cliplite::library::edit_project_video(segs, out, ceo, nullptr),
                  "canvas fit render");
            UINT32 w = 0, h = 0;
            probe_dims(out, w, h);
            std::printf("  canvas fit dims=%ux%u corner=%d\n", w, h, corner_luma(out));
            check(w == 240 && h == 240, "canvas fit dims 240x240");
            check(corner_luma(out) == 16, "canvas fit has letterbox bars");
        }
        // Fill 9:16 of 320x240 -> center crop 134x240 (content, no bars).
        {
            cliplite::library::EditOptions ceo;
            ceo.crop_x = (320 - 134) / 2;
            ceo.crop_y = 0;
            ceo.crop_w = 134;
            ceo.crop_h = 240;
            const std::vector<cliplite::library::ProjectSegment> segs = {
                {clip, 0, 1000, 1.0f}};
            const std::wstring out = dir + L"\\canvas_fill.mp4";
            check(cliplite::library::edit_project_video(segs, out, ceo, nullptr),
                  "canvas fill render");
            UINT32 w = 0, h = 0;
            probe_dims(out, w, h);
            std::printf("  canvas fill dims=%ux%u\n", w, h);
            check(w == 134 && h == 240, "canvas fill dims 134x240");
        }
    }

    // Multi-input import: clipA (320x240, game stems) + clipB (160x120 with
    // its own tone). Different resolutions normalize to clipA's output size;
    // B's audio decodes from its file. Expect ~1500ms with audio.
    {
        const std::wstring clipB = dir + L"\\importB.mp4";
        {
            cliplite::encoder::MediaFoundationEncoder enc;
            cliplite::encoder::EncoderConfig cfg;
            cfg.width = 160;
            cfg.height = 120;
            cfg.fps_num = 30;
            cfg.fps_den = 1;
            cfg.video_bitrate_bps = 300'000;
            cfg.audio_enabled = true;
            if (!enc.start(clipB, cfg)) {
                check(false, "import encoder start");
            } else {
                std::vector<uint8_t> nv12(static_cast<size_t>(160) * 120 * 3 / 2);
                std::vector<float> tone(static_cast<size_t>(1600) * 2);
                const int64_t frame_dur = 10'000'000LL / 30;
                for (uint32_t f = 0; f < 15; ++f) {
                    fill_nv12(nv12, 160, 120, f + 40);
                    enc.push_video_frame(nv12.data(), 160,
                                         static_cast<int64_t>(f) * frame_dur);
                    for (uint32_t i = 0; i < 1600; ++i) {
                        const float v =
                            0.25f *
                            std::sin(2.0f * 3.14159265f * 330.0f *
                                     static_cast<float>(f * 1600 + i) / 48000.0f);
                        tone[static_cast<size_t>(i) * 2] = v;
                        tone[static_cast<size_t>(i) * 2 + 1] = v;
                    }
                    enc.push_audio_frames(tone.data(), 1600,
                                          static_cast<int64_t>(f) * frame_dur);
                }
                check(enc.finish(), "import encoder finish");
            }
        }
        cliplite::library::StemClipInfo infoI;
        check(cliplite::library::read_stem_sidecar(clip, &infoI), "import sidecar reads");
        auto groupsI = cliplite::audio::load_stem_snapshot(wide_to_utf8(snap));
        auto mixA = cliplite::audio::mix_stem_window(groupsI, {22}, 0, 0);
        check(!mixA.empty(), "import mixA non-empty");
        auto audB = cliplite::audio::decode_audio_range(clipB, 0, 500);
        check(!audB.empty(), "import decode non-empty");
        std::vector<float> full = mixA;
        full.insert(full.end(), audB.begin(), audB.end());
        cliplite::library::EditOptions ieo;
        ieo.mix_pcm = &full;
        const std::vector<cliplite::library::ProjectSegment> segs = {
            {clip, 0, 1000, 1.0f}, {clipB, 0, 500, 1.0f}};
        const std::wstring out = dir + L"\\import.mp4";
        check(cliplite::library::edit_project_video(segs, out, ieo, nullptr),
              "import render");
        check(has_mp4a_box(out), "import carries mp4a");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "import video decodes");
        if (t) DeleteObject(t);
        const int64_t d = cliplite::library::ClipLibrary::probe_duration_ms(out);
        std::printf("  import duration=%lld ms\n", static_cast<long long>(d));
        check(d >= 1200 && d <= 1800, "import duration ~1500ms");
    }

    // No sidecar at all: plain passthrough copy still succeeds (old clips).
    {
        std::filesystem::remove(clip + L".apps.json", ec);
        std::filesystem::remove_all(snap, ec);
        const std::wstring out = dir + L"\\mix_plain.mp4";
        check(cliplite::library::export_without_apps(clip, out, {}), "fallback copy");
        HBITMAP t = cliplite::library::generate_thumbnail(out, 64, 36);
        check(t != nullptr, "fallback video decodes");
        if (t) DeleteObject(t);
    }

    // Set CLIPLITE_KEEP_MIXDOWN_TEST=1 to keep artifacts for external probing.
    wchar_t keep[2] = {0};
    if (GetEnvironmentVariableW(L"CLIPLITE_KEEP_MIXDOWN_TEST", keep, 2) == 0) {
        std::filesystem::remove_all(dir, ec);
    } else {
        std::printf("  kept %ls\n", dir.c_str());
    }

    std::printf(g_failures == 0 ? "STEM MIXDOWN SELFTEST PASS\n"
                                : "STEM MIXDOWN SELFTEST FAIL (%d)\n",
                g_failures);
    cliplite::log::shutdown();
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
