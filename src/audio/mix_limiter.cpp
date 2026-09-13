#include "cliplite/audio/mix_limiter.h"

#include <algorithm>
#include <cmath>

namespace cliplite::audio {

void limit_mix_block(float* pcm, uint32_t frames, uint16_t channels, MixLimiter& st) {
    if (!pcm || frames == 0 || channels == 0) return;
    const size_t n = static_cast<size_t>(frames) * channels;
    float peak = 0.f;
    for (size_t i = 0; i < n; ++i) {
        const float a = std::fabs(pcm[i]);
        if (a > peak) peak = a;
    }
    if (peak > 1.f) {
        st.gain = 1.f / peak;  // instant attack: fit this block exactly
    } else if (st.gain < 1.f) {
        st.gain += (1.f - st.gain) * 0.15f;  // gentle release
        if (st.gain > 1.f) st.gain = 1.f;
        if (st.gain > 1.f - 1e-6f) st.gain = 1.f;
    }
    if (st.gain >= 1.f) return;  // bit-identical fast path (also covers peak<=1)
    for (size_t i = 0; i < n; ++i) pcm[i] *= st.gain;
}

}  // namespace cliplite::audio
