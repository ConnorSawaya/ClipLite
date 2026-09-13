// WASAPI loopback self-test: captures desktop audio, optionally detects the
// Beep() tone it plays, and reports the negotiated format.

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "cliplite/audio/wasapi_loopback.h"
#include "cliplite/log.h"

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cliplite::log::init("cliplite_audio_test.log");
    int failures = 0;

    cliplite::audio::WasapiLoopback loop;
    cliplite::audio::AudioFormat fmt;
    if (!loop.start(&fmt)) {
        std::printf("FAIL wasapi loopback start\n");
        ++failures;
    } else {
        std::printf("PASS loopback start: %u Hz, %u ch, %u-bit\n", fmt.sample_rate,
                    fmt.channels, fmt.bits_per_sample);

        // Play a tone so there is real signal to capture.
        std::thread beeper([]() {
            Sleep(200);
            Beep(880, 400);
        });

        const uint32_t total = fmt.sample_rate;  // ~1 second
        std::vector<float> buf(static_cast<size_t>(total) * fmt.channels);
        uint32_t got = 0;
        const ULONGLONG t0 = GetTickCount64();
        while (got < total && (GetTickCount64() - t0) < 1500) {
            got += loop.read(buf.data() + static_cast<size_t>(got) * fmt.channels, total - got);
            Sleep(10);
        }
        beeper.join();

        float peak = 0.0f;
        for (uint32_t i = 0; i < got * fmt.channels; ++i)
            peak = std::max(peak, std::fabs(buf[i]));

        std::printf("captured %u frames, peak=%.4f\n", got, peak);
        if (got == 0) {
            std::printf("FAIL no frames captured\n");
            ++failures;
        } else if (peak > 0.01f) {
            std::printf("PASS captured non-silent audio\n");
        } else {
            std::printf("PASS captured audio (silent system)\n");
        }
        loop.stop();
    }

    std::printf(failures == 0 ? "AUDIO SELFTEST PASS\n" : "AUDIO SELFTEST FAIL (%d failures)\n",
                failures);
    cliplite::log::shutdown();
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
