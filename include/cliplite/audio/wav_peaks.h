#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cliplite::audio {

// Streaming per-bucket peakabs (0..1) over a WAV file (float32 or PCM16,
// any rate/channels; channels are maxed). O(buckets) RAM regardless of file
// size: reads in small chunks, never loads the whole file. Empty on any
// failure (missing file, bad header, buckets == 0).
std::vector<float> compute_wav_peaks(const std::string& utf8_path, uint32_t buckets);

// Header-only probe (no audio decoded). ok=false unless float32/PCM16.
struct WavInfo {
    bool ok = false;
    uint32_t frames = 0;
    uint32_t rate = 0;
    uint16_t channels = 0;
};
WavInfo wav_info(const std::string& utf8_path);

// "<k>.app_<pid>.wav" grouping for a snapshot dir: k -> pid -> path.
// Malformed names are skipped; nothing is read.
std::map<uint32_t, std::map<uint32_t, std::string>> group_snapshot_paths(
    const std::string& utf8_dir);

// Peaks across one pid's ordered k-files sharing a timeline: per-file bucket
// counts proportional to frame counts, concatenated to exactly `buckets`
// (each file streams, so RAM stays O(buckets)). Empty when nothing usable.
std::vector<float> concat_pid_peaks(const std::map<uint32_t, std::string>& k_files,
                                    uint32_t buckets);

}  // namespace cliplite::audio
