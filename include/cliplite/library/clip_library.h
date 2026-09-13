#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cliplite::library {

struct ClipEntry {
    std::wstring path;
    std::wstring filename;
    std::string game;      // derived from the filename prefix
    uint64_t file_size = 0;
    int64_t duration_ms = 0;  // 0 when unknown
};

// Scans a clip directory and exposes lightweight metadata. Durations are probed
// lazily per clip via probe_duration_ms (moov read, no decode).
class ClipLibrary {
public:
    // Lists *.mp4 files, newest first (filenames encode timestamps).
    void scan(const std::wstring& directory);

    const std::vector<ClipEntry>& clips() const { return clips_; }
    std::size_t size() const { return clips_.size(); }

    // Reads a clip's duration (ms) from its container metadata. Returns 0 on
    // failure.
    static int64_t probe_duration_ms(const std::wstring& path);

private:
    std::vector<ClipEntry> clips_;
};

}  // namespace cliplite::library
