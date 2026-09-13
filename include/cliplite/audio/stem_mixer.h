#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cliplite::audio {

// Mixes per-app PCM stems with all-selected-by-default removal semantics.
// Pure logic, no Win32/COM: the recorder pushes each app's drained frames per
// tick and takes mix_available() for the encoder.
//
// Alignment policy (documented approximation for the live path): each call
// mixes up to the frames queued in every INCLUDED stem that currently holds
// data; leftovers persist and are consumed first later. A stem that delivers
// nothing on a tick contributes silence for that tick (it is skipped, not
// waited on), so one dry/exited app can never stall the mix. Sample values
// are energy-preserving: every pushed frame is mixed exactly once, in order.
// Exact sample-accurate post-clip re-renders align recorded stem files by
// timeline instead of using this live mixer.
class StemMixer {
public:
    struct Mixed {
        std::vector<float> pcm;  // interleaved, clamped to [-1,1]
        uint32_t frames = 0;
    };

    explicit StemMixer(uint16_t channels = 2, uint32_t max_buffered_frames = 96000);

    // Queue drained frames for one app (pid identifies the stem).
    void push(uint32_t pid, const std::string& exe, const float* pcm, uint32_t frames);
    // Exclude/include apps. Default: nothing excluded (full mix = old behavior).
    void set_excluded(const std::set<uint32_t>& pids);
    const std::set<uint32_t>& excluded() const { return excluded_; }
    // Mix queued audio; see policy above. Never waits, never loses data
    // except oldest-first drops past the per-stem cap (bounded memory).
    Mixed mix_available();
    // pid -> exe for stems currently known (queued or not).
    std::map<uint32_t, std::string> stems() const;

    uint16_t channels() const { return channels_; }
    void clear();

private:
    struct Stem {
        std::string exe;
        std::vector<float> fifo;  // interleaved samples awaiting mix
    };
    uint16_t channels_;
    uint32_t cap_frames_;
    std::map<uint32_t, Stem> stems_;
    std::set<uint32_t> excluded_;
};

}  // namespace cliplite::audio
