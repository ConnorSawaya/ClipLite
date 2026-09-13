#include "test_main.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "cliplite/audio/stem_writer.h"
#include "cliplite/audio/wav_peaks.h"

using namespace cliplite::audio;

namespace {
std::string tmp_wav(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}
}  // namespace

TEST(wav_peaks_constant) {
    const std::string path = tmp_wav("cliplite_peaks_const.wav");
    std::remove(path.c_str());
    StemSegmentWriter w;
    CHECK(w.open(path));
    std::vector<float> pcm(static_cast<size_t>(4800) * 2, 0.4f);
    CHECK(w.append(pcm.data(), 4800));
    CHECK(w.close());
    auto peaks = compute_wav_peaks(path, 24);
    CHECK_EQ(peaks.size(), 24u);
    for (float p : peaks) CHECK(near(p, 0.4f));
    std::remove(path.c_str());
}

TEST(wav_peaks_sine_and_ramp) {
    const std::string path = tmp_wav("cliplite_peaks_sine.wav");
    std::remove(path.c_str());
    StemSegmentWriter w;
    CHECK(w.open(path));
    // 1s: first half silence, second half 0.5-amplitude sine.
    std::vector<float> pcm(static_cast<size_t>(48000) * 2, 0.f);
    for (uint32_t f = 24000; f < 48000; ++f) {
        const float v =
            0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / 48000.0f);
        pcm[static_cast<size_t>(f) * 2] = v;
        pcm[static_cast<size_t>(f) * 2 + 1] = v;
    }
    CHECK(w.append(pcm.data(), 48000));
    CHECK(w.close());
    auto peaks = compute_wav_peaks(path, 10);
    CHECK_EQ(peaks.size(), 10u);
    for (int i = 0; i < 5; ++i) CHECK(peaks[static_cast<size_t>(i)] == 0.f);
    for (int i = 5; i < 10; ++i) CHECK(peaks[static_cast<size_t>(i)] > 0.49f);
    std::remove(path.c_str());
}

TEST(wav_info_reads_header_only) {
    const std::string path = tmp_wav("cliplite_peaks_info.wav");
    std::remove(path.c_str());
    StemSegmentWriter w;
    CHECK(w.open(path));
    std::vector<float> pcm(static_cast<size_t>(1000) * 2, 0.1f);
    CHECK(w.append(pcm.data(), 1000));
    CHECK(w.close());
    const WavInfo info = wav_info(path);
    CHECK(info.ok);
    CHECK_EQ(info.frames, 1000u);
    CHECK_EQ(info.rate, 48000u);
    CHECK_EQ(info.channels, 2u);
    CHECK(!wav_info("").ok);
    CHECK(!wav_info(tmp_wav("nope-missing.wav")).ok);
    std::remove(path.c_str());
}

TEST(concat_pid_peaks_stitches_timeline) {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "cliplite_concat_test").string();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    // k=0: silence (2400 frames), k=1: 0.5 tone (2400 frames).
    std::vector<float> quiet(static_cast<size_t>(2400) * 2, 0.f);
    std::vector<float> tone(static_cast<size_t>(2400) * 2);
    for (uint32_t f = 0; f < 2400; ++f) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / 48000.0f);
        tone[static_cast<size_t>(f) * 2] = v;
        tone[static_cast<size_t>(f) * 2 + 1] = v;
    }
    StemSegmentWriter a, b;
    CHECK(a.open(dir + "/0.app_11.wav"));
    CHECK(a.append(quiet.data(), 2400));
    CHECK(a.close());
    CHECK(b.open(dir + "/1.app_11.wav"));
    CHECK(b.append(tone.data(), 2400));
    CHECK(b.close());
    { std::ofstream junk(dir + "/notes.txt"); junk << "skip me"; }

    auto groups = group_snapshot_paths(dir);
    CHECK_EQ(groups.size(), 2u);
    CHECK_EQ(groups[0].size(), 1u);
    CHECK(groups[0].count(11) == 1);
    CHECK(group_snapshot_paths("").empty());
    CHECK(group_snapshot_paths("Z:\\definitely\\not\\here").empty());

    std::map<uint32_t, std::string> files;
    for (const auto& [k, m] : groups) files[k] = m.at(11);
    auto peaks = concat_pid_peaks(files, 10);
    CHECK_EQ(peaks.size(), 10u);
    for (int i = 0; i < 5; ++i) CHECK(peaks[static_cast<size_t>(i)] == 0.f);
    for (int i = 5; i < 10; ++i) CHECK(peaks[static_cast<size_t>(i)] > 0.49f);
    CHECK(concat_pid_peaks({}, 10).empty());
    CHECK(concat_pid_peaks(files, 0).empty());
    std::filesystem::remove_all(dir, ec);
}

TEST(wav_peaks_rejects_garbage) {
    CHECK(compute_wav_peaks("", 10).empty());
    CHECK(compute_wav_peaks(tmp_wav("nope-missing.wav"), 10).empty());
    const std::string path = tmp_wav("cliplite_peaks_empty.wav");
    std::remove(path.c_str());
    StemSegmentWriter w;
    CHECK(w.open(path));
    CHECK(w.close());  // header-only, zero frames
    CHECK(compute_wav_peaks(path, 10).empty());
    CHECK(compute_wav_peaks(path, 0).empty());
    { std::ofstream f(tmp_wav("cliplite_peaks_junk.wav")); f << "junkjunkjunkjunkjunkjunkjunkjunkjunkjunkjunk"; }
    CHECK(compute_wav_peaks(tmp_wav("cliplite_peaks_junk.wav"), 10).empty());
    std::remove(path.c_str());
    std::remove(tmp_wav("cliplite_peaks_junk.wav").c_str());
}
