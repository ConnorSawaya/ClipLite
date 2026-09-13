#include "test_main.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include "cliplite/audio/stem_track.h"
#include "cliplite/audio/stem_writer.h"

using namespace cliplite::audio;

namespace {
std::string tmp_dir(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
bool near(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}
double rms_of(const std::vector<float>& v) {
    double sum = 0;
    for (float s : v) sum += static_cast<double>(s) * s;
    return v.empty() ? 0 : std::sqrt(sum / v.size());
}
}  // namespace

TEST(stem_track_full_mix_is_exact_sum) {
    const std::string dir = tmp_dir("cliplite_stem_track_test");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    std::vector<float> game(static_cast<size_t>(4800) * 2);
    std::vector<float> spot(static_cast<size_t>(4800) * 2, 0.3f);
    for (uint32_t f = 0; f < 4800; ++f) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / 48000.0f);
        game[static_cast<size_t>(f) * 2] = v;
        game[static_cast<size_t>(f) * 2 + 1] = v;
    }
    StemSegmentWriter wg, ws;
    CHECK(wg.open(dir + "/0.app_11.wav"));
    CHECK(wg.append(game.data(), 4800));
    CHECK(wg.close());
    CHECK(ws.open(dir + "/0.app_22.wav"));
    CHECK(ws.append(spot.data(), 4800));
    CHECK(ws.close());

    auto groups = load_stem_snapshot(dir);
    CHECK_EQ(groups.size(), 1u);
    CHECK_EQ(groups[0].stems.size(), 2u);

    // Full mix, no codec in the loop: exact analytic RMS.
    auto full = mix_stem_window(groups, {}, 0, 0);
    CHECK_EQ(full.size(), game.size());
    CHECK(near(static_cast<float>(rms_of(full)), 0.4637f, 0.001f));

    // Excluding spot leaves the game stem bit-exact.
    auto nospo = mix_stem_window(groups, {22}, 0, 0);
    CHECK_EQ(nospo.size(), game.size());
    CHECK(near(static_cast<float>(rms_of(nospo)), 0.3536f, 0.001f));
    for (size_t i = 0; i < game.size(); ++i) CHECK(nospo[i] == game[i]);

    // Trims cut exact frame counts (10ms head + 20ms tail of the 100ms stem).
    auto trimmed = mix_stem_window(groups, {}, 10, 20);
    CHECK_EQ(trimmed.size(), static_cast<size_t>(4800 - 480 - 960) * 2u);
    // Content lines up with the untrimmed mix past the head cut.
    for (size_t i = 0; i < trimmed.size(); ++i) CHECK(trimmed[i] == full[i + 480u * 2u]);
    // Over-trimming yields empty, never underflow.
    CHECK(mix_stem_window(groups, {}, 100, 200).empty());

    std::filesystem::remove_all(dir, ec);
}

TEST(mix_snapshot_streaming_matches_load_all) {
    const std::string dir = tmp_dir("cliplite_stream_test");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    // k=0: game tone + spot const; k=1: game tone only (uneven lengths).
    std::vector<float> g0(static_cast<size_t>(4800) * 2);
    std::vector<float> s0(static_cast<size_t>(4800) * 2, 0.3f);
    std::vector<float> g1(static_cast<size_t>(2400) * 2);
    for (uint32_t f = 0; f < 4800; ++f) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * f / 48000.0f);
        g0[static_cast<size_t>(f) * 2] = v;
        g0[static_cast<size_t>(f) * 2 + 1] = v;
    }
    for (uint32_t f = 0; f < 2400; ++f) {
        const float v = 0.4f * std::sin(2.0f * 3.14159265f * 330.0f * f / 48000.0f);
        g1[static_cast<size_t>(f) * 2] = v;
        g1[static_cast<size_t>(f) * 2 + 1] = v;
    }
    StemSegmentWriter w;
    CHECK(w.open(dir + "/0.app_11.wav"));
    CHECK(w.append(g0.data(), 4800));
    CHECK(w.close());
    CHECK(w.open(dir + "/0.app_22.wav"));
    CHECK(w.append(s0.data(), 4800));
    CHECK(w.close());
    CHECK(w.open(dir + "/1.app_11.wav"));
    CHECK(w.append(g1.data(), 2400));
    CHECK(w.close());
    { std::ofstream junk(dir + "/notes.txt"); junk << "skip me"; }

    const std::map<uint32_t, bool> known = {{11, true}, {22, true}};
    // Reference: load-all + mix (10/20ms trims fit the 150ms window).
    auto groups = load_stem_snapshot(dir);
    auto ref = mix_stem_window(groups, {22}, 10, 20);
    groups.clear();  // groups must be freeable before streaming matters
    auto streamed = mix_snapshot_streaming(dir, known, {22}, 10, 20);
    CHECK_EQ(streamed.size(), ref.size());
    CHECK(!streamed.empty());
    for (size_t i = 0; i < ref.size(); ++i) CHECK(streamed[i] == ref[i]);
    // No filter map accepts everything found.
    auto streamed2 = mix_snapshot_streaming(dir, {}, {22}, 10, 20);
    CHECK_EQ(streamed2.size(), ref.size());
    for (size_t i = 0; i < ref.size(); ++i) CHECK(streamed2[i] == ref[i]);
    // Excluding everything yields empty; garbage dir yields empty.
    CHECK(mix_snapshot_streaming(dir, known, {11, 22}, 0, 0).empty());
    CHECK(mix_snapshot_streaming("", known, {}, 0, 0).empty());
    CHECK(mix_snapshot_streaming("Z:\\definitely\\not\\here", known, {}, 0, 0).empty());
    std::filesystem::remove_all(dir, ec);
}

TEST(mix_snapshot_gains_scale_per_app) {
    const std::string dir = tmp_dir("cliplite_gains_test");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    // game = const 0.4, spot = const 0.4, one k-slice each.
    std::vector<float> g(static_cast<size_t>(4800) * 2, 0.4f);
    std::vector<float> s(static_cast<size_t>(4800) * 2, 0.4f);
    StemSegmentWriter a, b;
    CHECK(a.open(dir + "/0.app_11.wav"));
    CHECK(a.append(g.data(), 4800));
    CHECK(a.close());
    CHECK(b.open(dir + "/0.app_22.wav"));
    CHECK(b.append(s.data(), 4800));
    CHECK(b.close());
    const std::map<uint32_t, bool> known = {{11, true}, {22, true}};
    // Full gains: 0.4+0.4 = 0.8 everywhere.
    auto full = mix_snapshot_streaming_gains(dir, known, {}, 0, 0);
    CHECK(!full.empty());
    for (float v : full) CHECK(near(v, 0.8f));
    // Half Spotify: 0.4 + 0.2 = 0.6.
    auto half = mix_snapshot_streaming_gains(dir, known, {{22, 0.5f}}, 0, 0);
    CHECK_EQ(half.size(), full.size());
    for (float v : half) CHECK(near(v, 0.6f));
    // Zero gain == excluded.
    auto mute = mix_snapshot_streaming_gains(dir, known, {{22, 0.f}}, 0, 0);
    auto excl = mix_snapshot_streaming(dir, known, {22}, 0, 0);
    CHECK_EQ(mute.size(), excl.size());
    for (size_t i = 0; i < mute.size(); ++i) CHECK(mute[i] == excl[i]);
    for (float v : mute) CHECK(near(v, 0.4f));
    std::filesystem::remove_all(dir, ec);
}

TEST(resample_pcm_linear_speed) {
    // 10-frame stereo ramp 0..9 per channel.
    std::vector<float> pcm(20);
    for (int i = 0; i < 10; ++i) {
        pcm[static_cast<size_t>(i) * 2] = static_cast<float>(i);
        pcm[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(i);
    }
    CHECK(resample_pcm_linear(pcm, 2, 1.0) == pcm);
    auto fast = resample_pcm_linear(pcm, 2, 2.0);
    CHECK_EQ(fast.size(), static_cast<size_t>(10));  // 5 frames stereo
    CHECK(near(fast[0], 0.f));
    CHECK(near(fast[8], 8.f));
    auto slow = resample_pcm_linear(pcm, 2, 0.5);
    CHECK_EQ(slow.size(), static_cast<size_t>(40));  // 20 frames stereo
    CHECK(near(slow[0], 0.f));
    CHECK(near(slow[2], 0.5f));  // interpolated midpoint
    CHECK(resample_pcm_linear(pcm, 2, 0.0).empty());
    CHECK(resample_pcm_linear({}, 2, 2.0).empty());
    CHECK(resample_pcm_linear(pcm, 0, 2.0).empty());
}

TEST(apply_fade_inout_ramps) {
    // Constant 1.0 mono: fade 4 in + 4 out over 10 frames.
    std::vector<float> pcm(10, 1.0f);
    apply_fade_inout(pcm, 1, 4, 4);
    CHECK(near(pcm[0], 1.f / 5));
    CHECK(near(pcm[3], 4.f / 5));
    CHECK(near(pcm[4], 1.f));
    CHECK(near(pcm[5], 1.f));
    CHECK(near(pcm[6], 4.f / 5));
    CHECK(near(pcm[9], 1.f / 5));
    // Zero fades are a no-op; empty is safe.
    std::vector<float> q(4, 0.5f);
    apply_fade_inout(q, 1, 0, 0);
    for (float v : q) CHECK(v == 0.5f);
    std::vector<float> empty;
    apply_fade_inout(empty, 2, 10, 10);
    CHECK(empty.empty());
    apply_fade_inout(q, 0, 2, 2);
    for (float v : q) CHECK(v == 0.5f);
}

TEST(stem_track_skips_garbage) {
    const std::string dir = tmp_dir("cliplite_stem_track_garbage");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    { std::ofstream f(dir + "/notes.txt"); f << "not a stem"; }
    { std::ofstream f(dir + "/0.app_9.wav"); f << "too short"; }
    auto groups = load_stem_snapshot(dir);
    CHECK(groups.empty());
    auto mixed = mix_stem_window(groups, {}, 0, 0);
    CHECK(mixed.empty());
    CHECK(load_stem_snapshot("").empty());
    CHECK(load_stem_snapshot("Z:\\definitely\\not\\here").empty());
    std::filesystem::remove_all(dir, ec);
}
