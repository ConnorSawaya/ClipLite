#pragma once

#include <cstdint>

namespace cliplite::audio {

// Transparent peak limiter for mixed blocks. Quiet blocks pass bit-identical
// (gain rests at 1.0); blocks peaking past full-scale are scaled to fit,
// preserving waveform shape instead of flat-top hard-clipping (which reads as
// harsh garble, especially game+mic sums). Instant attack, gentle release:
// gain drops immediately to fit a hot block, then recovers ~15% per call
// toward 1.0 while blocks stay under threshold.
struct MixLimiter {
    float gain = 1.f;
};

void limit_mix_block(float* pcm, uint32_t frames, uint16_t channels, MixLimiter& st);

}  // namespace cliplite::audio
