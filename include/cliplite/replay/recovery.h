#pragma once

#include <cstdint>
#include <string>

namespace cliplite::replay {

// Deletes orphaned temporary segment files left in the buffer directory by a
// previous crash (anything matching segment_*.tmp.mp4). Call at startup.
void cleanup_buffer(const std::wstring& buffer_dir);

// Free space (bytes) on the volume containing `path`, or -1 on failure.
int64_t free_disk_bytes(const std::wstring& path);

}  // namespace cliplite::replay
