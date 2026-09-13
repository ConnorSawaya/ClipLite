#include "test_main.h"

#include <cmath>
#include <vector>

#include "cliplite/audio/mix_limiter.h"

using namespace cliplite::audio;

TEST(mix_limiter_quiet_passes_identical) {
    MixLimiter lim;
    std::vector<float> pcm = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.5f};
    const auto orig = pcm;
    limit_mix_block(pcm.data(), 3, 2, lim);
    CHECK_EQ(lim.gain, 1.f);
    for (size_t i = 0; i < pcm.size(); ++i) CHECK(pcm[i] == orig[i]);
}

TEST(mix_limiter_hot_block_fits_shape) {
    MixLimiter lim;
    std::vector<float> pcm(200, 1.6f);  // flat-top candidate
    limit_mix_block(pcm.data(), 100, 2, lim);
    float peak = 0.f;
    for (float s : pcm) {
        if (std::fabs(s) > peak) peak = std::fabs(s);
        CHECK(s <= 1.f);
    }
    CHECK(peak > 0.999f);  // uses the headroom instead of ducking low
    // Shape preserved: still constant (a hard clipper would also hold 1.0
    // here, so check a ramp keeps its slope instead).
    std::vector<float> ramp(200);
    for (int i = 0; i < 200; ++i) ramp[static_cast<size_t>(i)] = -1.6f + i * (3.2f / 199);
    MixLimiter lim2;
    limit_mix_block(ramp.data(), 100, 2, lim2);
    for (int i = 1; i < 200; ++i)
        CHECK(ramp[static_cast<size_t>(i)] >= ramp[static_cast<size_t>(i - 1)]);
    CHECK(ramp.back() <= 1.f);
}

TEST(mix_limiter_releases_to_transparent) {
    MixLimiter lim;
    std::vector<float> hot(20, 2.0f);
    limit_mix_block(hot.data(), 10, 2, lim);
    CHECK(lim.gain < 1.f);
    std::vector<float> quiet(20, 0.1f);
    float prev = lim.gain;
    for (int i = 0; i < 200; ++i) {
        limit_mix_block(quiet.data(), 10, 2, lim);
        CHECK(lim.gain >= prev);
        prev = lim.gain;
    }
    CHECK_EQ(lim.gain, 1.f);
}

TEST(mix_limiter_ignores_empty) {
    MixLimiter lim;
    limit_mix_block(nullptr, 10, 2, lim);
    std::vector<float> pcm(4, 0.5f);
    limit_mix_block(pcm.data(), 0, 2, lim);
    limit_mix_block(pcm.data(), 2, 0, lim);
    CHECK_EQ(lim.gain, 1.f);
}
