#pragma once

#include <audioclient.h>
#include <wrl/client.h>

#include <cstdint>

namespace cliplite::audio {

// Captures the audio of ONE process tree (pid + its child processes) via
// Windows process loopback in INCLUDE mode. Same float32-interleaved read()
// contract as WasapiLoopback, fixed at 48kHz stereo, so per-app stems mix
// identically to the full-mix loopback.
//
// Requirements: OS build 20348+ (Win11 21H2 / Server 2022; Win10 2004+
// unofficially). The target pid must exist when start() is called; if it
// exits, capture goes silent and the caller must re-resolve + restart.
// COM must be initialized on the calling thread (STA or MTA).
class ProcessLoopbackCapture {
public:
    ProcessLoopbackCapture() = default;
    ~ProcessLoopbackCapture();
    ProcessLoopbackCapture(const ProcessLoopbackCapture&) = delete;
    ProcessLoopbackCapture& operator=(const ProcessLoopbackCapture&) = delete;

    // Starts INCLUDE capture of target_pid's process tree. Returns false when
    // pid is 0, activation fails/times out, or the OS does not support it.
    bool start(uint32_t target_pid);
    void stop();
    bool running() const { return started_; }
    uint32_t target_pid() const { return target_pid_; }

    // Reads up to max_frames interleaved float frames. Returns frames read
    // (0 when the target is silent or has no audio streams).
    uint32_t read(float* dst, uint32_t max_frames);

    uint32_t sample_rate() const { return 48000; }
    uint16_t channels() const { return 2; }

private:
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    uint32_t target_pid_ = 0;
    bool started_ = false;
};

}  // namespace cliplite::audio
