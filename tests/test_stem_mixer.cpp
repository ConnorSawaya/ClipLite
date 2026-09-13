#include "test_main.h"

#include <cmath>

#include "cliplite/audio/stem_mixer.h"

using namespace cliplite::audio;

namespace {
std::vector<float> constant(uint32_t frames, float v, uint16_t ch = 2) {
    return std::vector<float>(static_cast<size_t>(frames) * ch, v);
}
bool near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}
}  // namespace

TEST(stem_mixer_all_selected_sums_by_default) {
    StemMixer m;
    auto a = constant(100, 0.25f);
    auto b = constant(100, 0.5f);
    m.push(11, "game.exe", a.data(), 100);
    m.push(22, "spotify.exe", b.data(), 100);
    auto out = m.mix_available();
    CHECK_EQ(out.frames, 100u);
    for (float s : out.pcm) CHECK(near(s, 0.75f));
}

TEST(stem_mixer_exclude_removes_app) {
    StemMixer m;
    auto a = constant(100, 0.25f);
    auto b = constant(100, 0.5f);
    m.push(11, "game.exe", a.data(), 100);
    m.push(22, "spotify.exe", b.data(), 100);
    m.set_excluded({22});
    auto out = m.mix_available();
    CHECK_EQ(out.frames, 100u);
    for (float s : out.pcm) CHECK(near(s, 0.25f));
    // Re-including restores the full mix (all-selected default).
    m.set_excluded({});
    m.push(11, "game.exe", a.data(), 100);
    m.push(22, "spotify.exe", b.data(), 100);
    auto full = m.mix_available();
    CHECK_EQ(full.frames, 100u);
    for (float s : full.pcm) CHECK(near(s, 0.75f));
}

TEST(stem_mixer_uneven_keeps_flowing) {
    StemMixer m;
    auto a = constant(100, 0.1f);
    auto b = constant(60, 0.2f);
    m.push(11, "game.exe", a.data(), 100);
    m.push(22, "spotify.exe", b.data(), 60);
    auto first = m.mix_available();
    CHECK_EQ(first.frames, 60u);
    for (float s : first.pcm) CHECK(near(s, 0.3f));
    // B is dry; A's 40 leftovers still flow (documented live policy).
    auto second = m.mix_available();
    CHECK_EQ(second.frames, 40u);
    for (float s : second.pcm) CHECK(near(s, 0.1f));
    auto third = m.mix_available();
    CHECK_EQ(third.frames, 0u);
}

TEST(stem_mixer_excluded_all_yields_silence) {
    StemMixer m;
    auto a = constant(10, 0.5f);
    m.push(11, "game.exe", a.data(), 10);
    m.set_excluded({11});
    auto out = m.mix_available();
    CHECK_EQ(out.frames, 0u);
    // Queued audio is retained, not destroyed, by exclusion.
    m.set_excluded({});
    auto back = m.mix_available();
    CHECK_EQ(back.frames, 10u);
}

TEST(stem_mixer_normalizes_hot_mix) {
    StemMixer m;
    // Ramp 0..1.5 plus const 0.5: sums 0.5..2.0, peak 2.0 -> scale 0.5.
    // A flat-top clipper would pin the entire top half to 1.0; scaling keeps
    // exactly one peak sample at 1.0 and the slope intact everywhere.
    std::vector<float> ramp(static_cast<size_t>(100) * 2);
    for (uint32_t f = 0; f < 100; ++f) {
        const float v = static_cast<float>(f) * (1.5f / 99);
        ramp[static_cast<size_t>(f) * 2] = v;
        ramp[static_cast<size_t>(f) * 2 + 1] = v;
    }
    auto b = constant(100, 0.5f);
    m.push(11, "a.exe", ramp.data(), 100);
    m.push(22, "b.exe", b.data(), 100);
    auto out = m.mix_available();
    CHECK_EQ(out.frames, 100u);
    for (float s : out.pcm) CHECK(s <= 1.0f);
    CHECK(near(out.pcm.front(), 0.25f));
    CHECK(near(out.pcm.back(), 1.0f));
    for (size_t i = 1; i < out.pcm.size(); ++i) CHECK(out.pcm[i] >= out.pcm[i - 1]);
}

TEST(stem_mixer_cap_drops_oldest) {
    StemMixer m(2, 100);  // 100-frame cap
    // Small ramp (no clamping) so survivors are identifiable by value.
    std::vector<float> ramp(static_cast<size_t>(300) * 2);
    for (uint32_t f = 0; f < 300; ++f) {
        ramp[static_cast<size_t>(f) * 2] = static_cast<float>(f) * 0.001f;
        ramp[static_cast<size_t>(f) * 2 + 1] = static_cast<float>(f) * 0.001f;
    }
    m.push(11, "a.exe", ramp.data(), 300);
    auto out = m.mix_available();
    CHECK_EQ(out.frames, 100u);
    // Only the newest 100 frames survived (200..299 -> 0.200..0.299).
    CHECK(near(out.pcm.front(), 0.200f));
    CHECK(near(out.pcm.back(), 0.299f));
}

TEST(stem_mixer_empty_and_stems_list) {
    StemMixer m;
    auto out = m.mix_available();
    CHECK_EQ(out.frames, 0u);
    CHECK(m.stems().empty());
    auto a = constant(5, 0.1f);
    m.push(11, "game.exe", a.data(), 5);
    CHECK_EQ(m.stems().size(), 1u);
    CHECK(m.stems()[11] == "game.exe");
    m.clear();
    CHECK(m.stems().empty());
}
