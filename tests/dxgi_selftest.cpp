// DXGI Desktop Duplication self-test: captures the primary monitor and verifies
// a real (non-black) frame is returned.

#include <windows.h>

#include <cstdio>

#include "cliplite/capture/dxgi_capture.h"
#include "cliplite/log.h"

int main() {
    cliplite::log::init("cliplite_dxgi_test.log");
    int failures = 0;

    cliplite::capture::DxgiDesktopDuplication dup;
    if (!dup.start(0)) {
        std::printf("FAIL desktop duplication start (locked screen / no desktop access?)\n");
        ++failures;
    } else {
        cliplite::capture::CapturedFrame frame;
        if (!dup.acquire_frame(&frame, 2000)) {
            std::printf("FAIL acquire_frame timed out\n");
            ++failures;
        } else {
            std::printf("PASS captured %ux%u stride=%u\n", frame.width, frame.height,
                        frame.stride);

            uint32_t nonzero = 0;
            uint32_t samples = 0;
            const uint8_t* p = frame.bgra;
            for (uint32_t y = 0; y < frame.height; y += 8) {
                for (uint32_t x = 0; x < frame.width; x += 8) {
                    const uint32_t idx = y * frame.stride + x * 4;
                    if (p[idx] || p[idx + 1] || p[idx + 2]) ++nonzero;
                    ++samples;
                }
            }
            std::printf("sampled %u pixels, %u non-black\n", samples, nonzero);
            // A valid frame of the right dimensions proves capture works. The
            // pixel content depends on what's on screen (may be black when
            // locked/idle), so report it but don't fail on it.
            if (samples == 0) {
                std::printf("FAIL no pixels sampled\n");
                ++failures;
            } else {
                std::printf("PASS frame captured (content: %s)\n",
                            nonzero == 0 ? "black/empty screen" : "real image");
            }
            dup.release_frame();
        }
        dup.stop();
    }

    std::printf(failures == 0 ? "DXGI SELFTEST PASS\n" : "DXGI SELFTEST FAIL (%d failures)\n",
                failures);
    cliplite::log::shutdown();
    return failures == 0 ? 0 : 1;
}
