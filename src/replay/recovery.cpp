#include "cliplite/replay/recovery.h"

#include <windows.h>

#include <filesystem>

#include "cliplite/log.h"

namespace cliplite::replay {

void cleanup_buffer(const std::wstring& buffer_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(buffer_dir, ec)) return;
    int removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(buffer_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring name = entry.path().filename().wstring();
        // Match our segment naming scheme: segment_<id>.tmp.mp4, plus
        // per-app stem sidecars: segment_<id>.app_<pid>.wav
        const bool is_segment = name.rfind(L"segment_", 0) == 0 && name.size() >= 12 &&
                                name.compare(name.size() - 8, 8, L".tmp.mp4") == 0;
        const bool is_stem = name.rfind(L"segment_", 0) == 0 &&
                             name.find(L".app_") != std::wstring::npos && name.size() >= 4 &&
                             name.compare(name.size() - 4, 4, L".wav") == 0;
        if (is_segment || is_stem) {
            std::filesystem::remove(entry.path(), ec);
            ++removed;
        }
    }
    if (removed > 0) CL_INFO("Recovery", "removed " + std::to_string(removed) + " orphan segment(s)");
}

int64_t free_disk_bytes(const std::wstring& path) {
    ULARGE_INTEGER free_avail{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER total_free{};
    if (GetDiskFreeSpaceExW(path.c_str(), &free_avail, &total, &total_free)) {
        return static_cast<int64_t>(free_avail.QuadPart);
    }
    return -1;
}

}  // namespace cliplite::replay
