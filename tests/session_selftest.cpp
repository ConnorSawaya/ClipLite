// Session monitor self-test: enumerates audible render sessions. Passes as long
// as enumeration works without crashing; silence (no audible apps) is a valid
// result on headless/CI machines, so the test asserts structure, not content.

#include <windows.h>
#include <objbase.h>

#include <cstdio>

#include "cliplite/audio/session_monitor.h"
#include "cliplite/log.h"

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cliplite::log::init("cliplite_session_test.log");
    int failures = 0;

    cliplite::audio::SessionMonitor mon;
    for (int round = 0; round < 2; ++round) {
        const auto apps = mon.poll_audible();
        std::printf("poll %d: %zu audible app(s)\n", round, apps.size());
        for (const auto& a : apps) {
            std::printf("  pid=%u exe=%s peak=%.3f\n", a.pid, a.exe.c_str(),
                        static_cast<double>(a.peak));
            if (a.exe.empty()) {
                std::printf("FAIL empty exe name\n");
                ++failures;
            }
            if (!(a.peak >= 0.f && a.peak <= 1.f)) {
                std::printf("FAIL peak out of range\n");
                ++failures;
            }
        }
        Sleep(300);
    }

    std::printf(failures == 0 ? "SESSION SELFTEST PASS\n" : "SESSION SELFTEST FAIL (%d)\n",
                failures);
    cliplite::log::shutdown();
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
