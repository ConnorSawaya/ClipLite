// Recovery self-test: orphan segment cleanup and free-disk query.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "cliplite/log.h"
#include "cliplite/replay/recovery.h"

namespace {
int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) std::printf("  ok: %s\n", what);
    else {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}
}  // namespace

int main() {
    cliplite::log::init("cliplite_recovery_test.log");

    const std::wstring dir = std::filesystem::temp_directory_path().wstring() + L"\\cliplite_recovery_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    std::printf("buffer cleanup\n");
    {
        std::ofstream(dir + L"\\segment_1.tmp.mp4").put('x');
        std::ofstream(dir + L"\\segment_2.tmp.mp4").put('x');
        std::ofstream(dir + L"\\segment_3.tmp.mp4").put('x');
        std::ofstream(dir + L"\\keepme.mp4").put('x');

        cliplite::replay::cleanup_buffer(dir);

        check(!std::filesystem::exists(dir + L"\\segment_1.tmp.mp4"), "removed segment_1");
        check(!std::filesystem::exists(dir + L"\\segment_2.tmp.mp4"), "removed segment_2");
        check(!std::filesystem::exists(dir + L"\\segment_3.tmp.mp4"), "removed segment_3");
        check(std::filesystem::exists(dir + L"\\keepme.mp4"), "kept non-segment file");
    }

    std::printf("free disk space\n");
    {
        const int64_t free_bytes = cliplite::replay::free_disk_bytes(dir);
        std::printf("  free on volume = %lld bytes\n", static_cast<long long>(free_bytes));
        check(free_bytes > 0, "free disk space > 0");
    }

    std::filesystem::remove_all(dir, ec);

    std::printf(g_failures == 0 ? "RECOVERY SELFTEST PASS\n" : "RECOVERY SELFTEST FAIL (%d)\n",
                g_failures);
    cliplite::log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
