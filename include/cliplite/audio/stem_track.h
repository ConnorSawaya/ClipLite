#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cliplite::audio {

// Snapshot stem I/O + window mixdown math. Pure logic (no Win32/MF): loads the
// float32 WAVs the recorder wrote, so removal is provable bit-exactly in unit
// tests without any audio codec in the loop. The Media Foundation mixdown
// renderer (library/stem_mixdown) calls these and only handles mux + encode.
struct StemGroup {
    uint32_t k = 0;  // chronological window position
    // pid -> interleaved PCM for that position (may differ in length).
    std::map<uint32_t, std::vector<float>> stems;
};

// Loads "<dir>/<k>.app_<pid>.wav" files. Malformed/unreadable files are
// skipped (empty groups omitted). Narrow UTF-8 dir API, portable.
std::map<uint32_t, StemGroup> load_stem_snapshot(const std::string& utf8_dir);

// Mixes one window: per-k max-pad across non-excluded pids, concatenated,
// then head/tail window trims (ms at sample_rate). Samples clamp to [-1,1].
// Empty when nothing usable is included.
std::vector<float> mix_stem_window(const std::map<uint32_t, StemGroup>& groups,
                                   const std::set<uint32_t>& excluded_pids,
                                   int64_t head_trim_ms, int64_t tail_trim_ms,
                                   uint32_t sample_rate = 48000,
                                   uint16_t channels = 2);

// Linear-interpolation PCM resampler for speed changes: speed > 1 shortens
// (fewer output frames), speed < 1 lengthens. Pure math, channel-agnostic.
// Returns empty for non-positive speed or empty input.
std::vector<float> resample_pcm_linear(const std::vector<float>& pcm, uint16_t channels,
                                       double speed);

// Linear fade in/out over interleaved PCM, in place. fades count in FRAMES
// (per channel); longer-than-content fades meet in the middle. No-op when
// both are 0 or pcm is empty.
void apply_fade_inout(std::vector<float>& interleaved, uint16_t channels,
                      uint32_t fade_in_frames, uint32_t fade_out_frames);

// Same result as load-all + mix_stem_window, but streams one k-position at a
// time: peak RAM is one k-slice of stems plus the output, never all stems at
// once. has_stem maps pid -> sidecar stem:true; an empty map accepts every pid
// found. Malformed files are skipped per-k.
std::vector<float> mix_snapshot_streaming(const std::string& utf8_dir,
                                          const std::map<uint32_t, bool>& has_stem,
                                          const std::set<uint32_t>& excluded_pids,
                                          int64_t head_trim_ms, int64_t tail_trim_ms,
                                          uint32_t sample_rate = 48000,
                                          uint16_t channels = 2);

// Gains variant: per-app linear gain (0.0 = removed, 1.0 = full, up to 2.0 =
// boost). Pids absent from gains play at 1.0; gain <= 0 behaves as excluded.
// Applied pre-sum per k-slice, then the same peak-normalize + trims.
std::vector<float> mix_snapshot_streaming_gains(
    const std::string& utf8_dir, const std::map<uint32_t, bool>& has_stem,
    const std::map<uint32_t, float>& gains, int64_t head_trim_ms, int64_t tail_trim_ms,
    uint32_t sample_rate = 48000, uint16_t channels = 2);

}  // namespace cliplite::audio
