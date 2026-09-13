#pragma once

#include <cstdint>
#include <string>

namespace cliplite::library {

// Losslessly exports [start_ms, end_ms] of `input` into a new MP4 at `output`
// by passing compressed H.264/AAC packets through Media Foundation (no decode,
// no re-encode). Works sample-accurately for clips recorded with ClipLite's
// all-keyframe encoder; other sources snap to the nearest earlier keyframe.
bool export_trimmed(const std::wstring& input, const std::wstring& output, int64_t start_ms,
                    int64_t end_ms);

}  // namespace cliplite::library
