#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cliplite::library {

// Streaming per-bucket peakabs (0..1) over a MEDIA file's audio (WAV/MP4/AAC:
// whatever the SourceReader decodes). O(buckets) RAM: one sample in flight,
// never the whole track. Video stream is deselected, so no frame is decoded.
// Empty on failure (no audio, buckets == 0, decode unavailable on the box).
std::vector<float> compute_media_peaks(const std::wstring& path, uint32_t buckets);

// Disk-cached wrapper (thumbs dir, path+mtime keyed). Second call for an
// unchanged file is a small JSON read, no decode.
std::vector<float> cached_media_peaks(const std::wstring& path, uint32_t buckets);

}  // namespace cliplite::library
