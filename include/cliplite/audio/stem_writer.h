#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace cliplite::audio {

// Writes one app stem as 32-bit float WAV (48kHz stereo, matching
// ProcessLoopbackCapture). The header is finalized on close(), so a crash
// loses at most the current segment's stem tail. Narrow UTF-8 path API keeps
// this portable (no Win32 in the header); the recorder converts its wide
// buffer paths before calling open().
class StemSegmentWriter {
public:
    StemSegmentWriter() = default;
    ~StemSegmentWriter();

    StemSegmentWriter(const StemSegmentWriter&) = delete;
    StemSegmentWriter& operator=(const StemSegmentWriter&) = delete;

    bool open(const std::string& utf8_path, uint32_t sample_rate = 48000,
              uint16_t channels = 2);
    bool append(const float* pcm, uint32_t frames);
    // Finalizes header sizes. Safe to call twice; no-op when never opened.
    bool close();

    bool is_open() const { return open_; }
    uint32_t frames_written() const { return frames_; }

private:
    std::ofstream out_;
    uint32_t sample_rate_ = 48000;
    uint16_t channels_ = 2;
    uint32_t frames_ = 0;
    bool open_ = false;
};

}  // namespace cliplite::audio
