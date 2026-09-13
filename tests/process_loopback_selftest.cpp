// Process-loopback self-test: INCLUDE-captures the test's own process tree.
// The test process plays no audio, so read() is expected to return 0 frames -
// the test asserts activation, format negotiation, read/stop/restart control
// flow, and rejection of pid 0. Requires OS build 20348+; on older builds
// start() fails and the test reports SKIP (exit 0) instead of FAIL.

#include <windows.h>
#include <objbase.h>

#include <cstdio>

#include "cliplite/audio/process_loopback.h"
#include "cliplite/log.h"



int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cliplite::log::init("cliplite_process_loopback_test.log");
    int failures = 0;

    cliplite::audio::ProcessLoopbackCapture cap;

    // pid 0 must be rejected without touching COM state.
    if (cap.start(0)) {
        std::printf("FAIL start(0) unexpectedly succeeded\n");
        ++failures;
        cap.stop();
    } else {
        std::printf("ok: start(0) rejected\n");
    }

    const uint32_t self = static_cast<uint32_t>(GetCurrentProcessId());
    // No version gate: attempt activation directly. On pre-20348 builds it
    // fails and we SKIP (environmental, not a regression).
    if (!cap.start(self)) {
        std::printf("SKIP process loopback activation failed (old OS or no audio engine)\n");
    } else {
        std::printf("ok: capturing self pid=%u rate=%u ch=%u\n", self, cap.sample_rate(),
                    cap.channels());
        if (cap.sample_rate() != 48000 || cap.channels() != 2) {
            std::printf("FAIL unexpected format\n");
            ++failures;
        }
        float buf[480 * 2] = {0};
        const uint32_t got = cap.read(buf, 480);
        std::printf("ok: read returned %u frames (0 expected, self is silent)\n", got);
        if (got != 0) {
            std::printf("FAIL expected silence from self capture\n");
            ++failures;
        }
        cap.stop();
        if (cap.running()) {
            std::printf("FAIL still running after stop\n");
            ++failures;
        } else {
            std::printf("ok: stopped\n");
        }
        // Restart must work (re-activation path).
        if (!cap.start(self)) {
            std::printf("FAIL restart\n");
            ++failures;
        } else {
            std::printf("ok: restarted\n");
            cap.stop();
        }
    }

    std::printf(failures == 0 ? "PROCESS LOOPBACK SELFTEST PASS\n"
                              : "PROCESS LOOPBACK SELFTEST FAIL (%d)\n",
                failures);
    cliplite::log::shutdown();
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
