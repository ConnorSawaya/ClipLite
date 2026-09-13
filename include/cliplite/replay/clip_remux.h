#pragma once

#include <string>
#include <vector>

namespace cliplite::replay {

// Concatenates H.264 + AAC MP4 files into a single MP4 without re-encoding
// (remux via Media Foundation source reader -> sink writer, compressed
// passthrough). Sample timestamps are rebased to be continuous across inputs.
// head_trim_ms drops that much wall-clock time from the start of inputs[0];
// tail_trim_ms drops that much from the end of inputs.back(). Both are
// best-effort (keyframe/wall-vs-media slop may leave <100ms error) but bound
// the previous +segment_ms over-capture to sub-second.
bool remux_mp4s(const std::vector<std::wstring>& inputs, const std::wstring& output,
                int64_t head_trim_ms = 0, int64_t tail_trim_ms = 0);

}  // namespace cliplite::replay
