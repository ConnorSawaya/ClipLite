// Replay recorder end-to-end test: captures the real desktop + loopback audio
// for ~4 s, then saves the previous 2 s as a clip and verifies the MP4.

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "cliplite/library/clip_library.h"
#include "cliplite/log.h"
#include "cliplite/replay/replay_recorder.h"

namespace {
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cliplite::log::init("cliplite_replay_test.log");
    int failures = 0;

    cliplite::replay::RecorderConfig cfg;
    cfg.fps = 15;
    cfg.video_bitrate_bps = 8'000'000;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.buffer_dir = L".";
    cfg.clip_dir = L".";
    cfg.segment_ms = 1000;
    cfg.replay_ms = 2000;
    cfg.stem_capture = true;  // exercise the per-app stem lifecycle (silent-safe)
    {
        // CLIPLITE_TEST_MIC=1 replicates mic-on user chains (mixing path).
        wchar_t mic[2] = {0};
        if (GetEnvironmentVariableW(L"CLIPLITE_TEST_MIC", mic, 2) != 0) {
            cfg.mic_enabled = true;
            std::printf("mic enabled for this run\n");
        }
    }

    cliplite::replay::ReplayRecorder rec;
    if (!rec.start(cfg)) {
        std::printf("FAIL recorder start\n");
        ++failures;
    } else {
        std::printf("recording %ux%u for ~4s\n", rec.width(), rec.height());
        const ULONGLONG t0 = GetTickCount64();
        POINT p0{};
        GetCursorPos(&p0);
        int ticks = 0;
        while (GetTickCount64() - t0 < 4000) {
            // Nudge the cursor to force desktop redraws (guarantees new frames).
            SetCursorPos(p0.x + (ticks % 40), p0.y + ((ticks / 40) % 2));
            if (!rec.tick()) {
                std::printf("FAIL tick\n");
                ++failures;
                break;
            }
            ++ticks;
            Sleep(16);
        }
        SetCursorPos(p0.x, p0.y);
        std::printf("ticks=%d buffered_ms=%lld\n", ticks, static_cast<long long>(rec.buffered_ms()));

        // Static-screen phase: no cursor nudges, no forced redraws. A healthy
        // recorder keeps the media timeline paced to wall clock here (gap
        // filled); without that, the saved window comes back far too short.
        const ULONGLONG t1 = GetTickCount64();
        while (GetTickCount64() - t1 < 2500) {
            if (!rec.tick()) {
                std::printf("FAIL tick (static phase)\n");
                ++failures;
                break;
            }
            ++ticks;
            Sleep(16);
        }
        std::printf("ticks=%d buffered_ms=%lld (after static phase)\n", ticks,
                    static_cast<long long>(rec.buffered_ms()));

        const std::wstring clip = rec.save_clip();
        if (clip.empty()) {
            std::printf("FAIL save_clip returned empty\n");
            ++failures;
        } else {
            const std::string path = wide_to_utf8(clip);
            std::printf("clip=%s\n", path.c_str());
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in) {
                std::printf("FAIL clip file missing\n");
                ++failures;
            } else {
                const std::streamoff size = in.tellg();
                in.seekg(0);
                char hdr[12] = {0};
                in.read(hdr, sizeof(hdr));
                const bool ftyp = std::memcmp(hdr + 4, "ftyp", 4) == 0;
                std::printf("clip size=%lld ftyp=%s\n", static_cast<long long>(size),
                            ftyp ? "yes" : "no");
                if (size < 500 || !ftyp) {
                    std::printf("FAIL clip invalid\n");
                    ++failures;
                } else {
                    std::printf("PASS clip written (verify with ffprobe)\n");
                }
                in.close();
            }
            // Length check: a 2000ms replay window must come back ~2000ms.
            // This is the regression test for short-clip reports.
            {
                const int64_t dur =
                    cliplite::library::ClipLibrary::probe_duration_ms(clip);
                std::printf("clip duration=%lld ms (want ~2000)\n",
                            static_cast<long long>(dur));
                if (dur < 1500 || dur > 2500) {
                    std::printf("FAIL clip length off\n");
                    ++failures;
                }
            }
            // Per-clip app sidecar: must exist (content may be [] on a silent
            // machine, so only existence + JSON shape are asserted here).
            const std::wstring sidecar = clip + L".apps.json";
            std::ifstream side(sidecar, std::ios::binary | std::ios::ate);
            if (!side) {
                std::printf("FAIL app sidecar missing\n");
                ++failures;
            } else {
                const std::streamoff n = side.tellg();
                side.seekg(0);
                std::string body(static_cast<size_t>(n > 0 ? n : 0), '\0');
                side.read(body.data(), n);
                const bool shape = body.rfind("{\"apps\":[", 0) == 0 &&
                                   !body.empty() && body.back() == '}';
                const bool stems_key = body.find("\"stems\":") != std::string::npos;
                std::printf("sidecar bytes=%lld shape=%s stems=%s\n",
                            static_cast<long long>(n), shape ? "yes" : "no",
                            stems_key ? "yes" : "no");
                if (!shape || !stems_key) {
                    std::printf("FAIL app sidecar malformed\n");
                    ++failures;
                }
                // Mic layer: a mic-enabled run must list the Microphone row
                // with a real stem, or editor remixes would silently drop the
                // voice (the bug this guards).
                {
                    wchar_t micenv[2] = {0};
                    if (GetEnvironmentVariableW(L"CLIPLITE_TEST_MIC", micenv, 2) != 0) {
                        const bool has_mic =
                            body.find("\"exe\":\"Microphone\"") != std::string::npos;
                        const bool mic_stem_true =
                            body.find("\"exe\":\"Microphone\"") != std::string::npos &&
                            body.find("\"stem\":true", body.find("\"exe\":\"Microphone\"")) !=
                                std::string::npos &&
                            body.find("\"stem\":true", body.find("\"exe\":\"Microphone\"")) <
                                body.find("}", body.find("\"exe\":\"Microphone\""));
                        std::printf("mic run: sidecar lists Microphone=%s stem=%s\n",
                                    has_mic ? "yes" : "no",
                                    mic_stem_true ? "true" : "false/other");
                        if (!has_mic || !mic_stem_true) {
                            std::printf("FAIL mic layer missing from sidecar\n");
                            ++failures;
                        }
                    }
                }
                side.close();
            }
            // Stem snapshot dir must exist next to the clip (may hold 0 wavs
            // on a silent machine, but the dir itself proves the path ran).
            const std::wstring snap_dir = clip.substr(0, clip.size() - 4) + L".stems";
            {
                std::error_code ec;
                if (!std::filesystem::is_directory(snap_dir, ec)) {
                    std::printf("FAIL stem snapshot dir missing\n");
                    ++failures;
                } else {
                    size_t wavs = 0;
                    bool mic_wav = false;
                    for (const auto& entry :
                         std::filesystem::directory_iterator(snap_dir, ec)) {
                        if (ec) break;
                        if (!entry.is_regular_file(ec)) continue;
                        ++wavs;
                        const std::wstring name = entry.path().filename().wstring();
                        if (name.find(L".app_4294967294.wav") != std::wstring::npos &&
                            entry.file_size(ec) > 44) {
                            mic_wav = true;
                        }
                    }
                    std::printf("ok: snapshot dir holds %zu wav(s), mic_stem=%s\n", wavs,
                                mic_wav ? "yes" : "no");
                    wchar_t micenv2[2] = {0};
                    if (GetEnvironmentVariableW(L"CLIPLITE_TEST_MIC", micenv2, 2) != 0 &&
                        !mic_wav) {
                        std::printf("FAIL mic stem wav missing\n");
                        ++failures;
                    }
                }
                wchar_t keep0[2] = {0};
                if (GetEnvironmentVariableW(L"CLIPLITE_KEEP_REPLAY_TEST", keep0, 2) == 0) {
                    std::filesystem::remove_all(snap_dir, ec);
                } else {
                    std::printf("kept %s\n", wide_to_utf8(snap_dir).c_str());
                }
            }
            wchar_t keep[2] = {0};
            const bool keep_clip =
                GetEnvironmentVariableW(L"CLIPLITE_KEEP_REPLAY_TEST", keep, 2) != 0;
            if (!keep_clip) {
                DeleteFileW(sidecar.c_str());
            } else {
                std::printf("kept %s\n", path.c_str());
                std::printf("kept %s\n", wide_to_utf8(sidecar).c_str());
            }
            bool removed = keep_clip;
            for (int attempt = 0; attempt < 10 && !removed; ++attempt) {
                removed = (DeleteFileW(clip.c_str()) != 0);
                if (!removed) Sleep(100);
            }
        }
        rec.stop();
        // Stem lifecycle leak check: no segment_*.app_*.wav may survive stop(),
        // whether or not any app was audible during the run. Skipped when the
        // caller asked to keep artifacts for inspection.
        wchar_t keep_leak[2] = {0};
        const bool keep_artifacts =
            GetEnvironmentVariableW(L"CLIPLITE_KEEP_REPLAY_TEST", keep_leak, 2) != 0;
        if (!keep_artifacts) {
            bool leaked = false;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(L".", ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;
                const std::wstring name = entry.path().filename().wstring();
                if (name.rfind(L"segment_", 0) == 0 &&
                    name.find(L".app_") != std::wstring::npos && name.size() >= 4 &&
                    name.compare(name.size() - 4, 4, L".wav") == 0) {
                    std::printf("FAIL leaked stem file %ls\n", name.c_str());
                    leaked = true;
                }
            }
            for (const auto& entry : std::filesystem::directory_iterator(L".", ec)) {
                if (ec) break;
                if (!entry.is_directory(ec)) continue;
                const std::wstring name = entry.path().filename().wstring();
                if (name.size() >= 6 &&
                    name.compare(name.size() - 6, 6, L".stems") == 0) {
                    std::printf("FAIL leaked snapshot dir %ls\n", name.c_str());
                    leaked = true;
                }
            }
            if (leaked) {
                ++failures;
            } else {
                std::printf("ok: no stem files leaked\n");
            }
        }
    }

    std::printf(failures == 0 ? "REPLAY SELFTEST PASS\n" : "REPLAY SELFTEST FAIL (%d failures)\n",
                failures);
    cliplite::log::shutdown();
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
