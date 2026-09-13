#pragma once

#include <audioclient.h>
#include <wrl/client.h>

#include <cstdint>

namespace cliplite::audio {

struct AudioFormat {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
};

// Captures the default render endpoint (desktop/game audio) via WASAPI loopback.
// Frames are delivered as interleaved float32 PCM regardless of the source format.
class WasapiLoopback {
public:
    WasapiLoopback() = default;
    ~WasapiLoopback();
    WasapiLoopback(const WasapiLoopback&) = delete;
    WasapiLoopback& operator=(const WasapiLoopback&) = delete;

    // Starts loopback capture on the default render device. On success, fills
    // out_format (if non-null) with the negotiated format.
    bool start(AudioFormat* out_format = nullptr);
    void stop();
    bool running() const { return started_; }

    // Reads up to max_frames interleaved float frames. Returns frames actually read.
    uint32_t read(float* dst, uint32_t max_frames);

    uint32_t sample_rate() const { return sample_rate_; }
    uint16_t channels() const { return channels_; }

private:
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX* mix_format_ = nullptr;
    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
    uint16_t bits_per_sample_ = 0;
    bool float_format_ = false;
    bool started_ = false;
};

}  // namespace cliplite::audio
