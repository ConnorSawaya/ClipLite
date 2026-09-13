#include "cliplite/audio/stem_mixer.h"

#include <algorithm>

namespace cliplite::audio {

StemMixer::StemMixer(uint16_t channels, uint32_t max_buffered_frames)
    : channels_(channels < 1 ? 1 : channels), cap_frames_(max_buffered_frames) {}

void StemMixer::push(uint32_t pid, const std::string& exe, const float* pcm, uint32_t frames) {
    if (!pcm || frames == 0 || channels_ == 0) return;
    Stem& stem = stems_[pid];
    if (stem.exe.empty()) stem.exe = exe;
    const size_t n = static_cast<size_t>(frames) * channels_;
    stem.fifo.insert(stem.fifo.end(), pcm, pcm + n);
    // Bound memory: drop oldest past the cap (a stem that permanently
    // outruns the mix is a bug upstream; never grow without bound).
    const size_t cap = static_cast<size_t>(cap_frames_) * channels_;
    if (stem.fifo.size() > cap) stem.fifo.erase(stem.fifo.begin(), stem.fifo.end() - cap);
}

void StemMixer::set_excluded(const std::set<uint32_t>& pids) {
    excluded_ = pids;
}

StemMixer::Mixed StemMixer::mix_available() {
    Mixed out;
    if (channels_ == 0) return out;
    // Mixable frames = min over included stems that currently hold data.
    uint32_t n = 0;
    bool any = false;
    for (const auto& [pid, stem] : stems_) {
        if (excluded_.count(pid)) continue;
        const uint32_t have = static_cast<uint32_t>(stem.fifo.size() / channels_);
        if (have == 0) continue;
        if (!any || have < n) n = have;
        any = true;
    }
    if (!any || n == 0) return out;

    out.pcm.assign(static_cast<size_t>(n) * channels_, 0.f);
    for (auto& [pid, stem] : stems_) {
        if (excluded_.count(pid)) continue;
        if (stem.fifo.size() < static_cast<size_t>(n) * channels_) continue;
        float* dst = out.pcm.data();
        const float* src = stem.fifo.data();
        const size_t count = static_cast<size_t>(n) * channels_;
        for (size_t i = 0; i < count; ++i) dst[i] += src[i];
    }
    // Peak-normalize hot chunks instead of flat-topping (see mix_limiter:
    // clipping harmonics read as garble; scaling preserves shape).
    float peak = 0.f;
    for (float s : out.pcm) {
        const float a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    if (peak > 1.f) {
        for (float& s : out.pcm) s /= peak;
    }
    // Consume in full from every stem that contributed (dry stems hold nothing
    // to consume, so no bookkeeping needed for them).
    for (auto& [pid, stem] : stems_) {
        if (excluded_.count(pid)) continue;
        const size_t count = static_cast<size_t>(n) * channels_;
        if (stem.fifo.size() >= count) stem.fifo.erase(stem.fifo.begin(), stem.fifo.begin() + count);
    }
    out.frames = n;
    return out;
}

std::map<uint32_t, std::string> StemMixer::stems() const {
    std::map<uint32_t, std::string> out;
    for (const auto& [pid, stem] : stems_) out[pid] = stem.exe;
    return out;
}

void StemMixer::clear() {
    stems_.clear();
    excluded_.clear();
}

}  // namespace cliplite::audio
