#pragma once

// File-audio decoding for timeline imports: reads [start_ms, end_ms) of any
// MF-decodable file as 48kHz stereo float PCM. Pure pipeline helper (Media
// Foundation dependent, lives in cliplite_media). Empty on failure, silence
// is NOT substituted — the caller decides what missing audio means.

#include <cstdint>
#include <string>
#include <vector>

namespace cliplite::audio {

std::vector<float> decode_audio_range(const std::wstring& path, int64_t start_ms,
                                      int64_t end_ms);

}  // namespace cliplite::audio
