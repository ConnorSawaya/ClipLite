#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cliplite::audio {

struct MicDeviceInfo {
    std::wstring id;    // endpoint id string (stable across sessions)
    std::string name;   // friendly name, e.g. "Microphone (Yeti)"
};

// Lists capture (microphone) endpoints.
std::vector<MicDeviceInfo> enumerate_mics();

// Default device id (empty on failure).
std::wstring default_mic_id();

// Shared-mode microphone capture producing float frames. Same contract as
// WasapiLoopback so the recorder can mix both identically.
class WasapiMic {
public:
    WasapiMic() = default;
    ~WasapiMic();
    WasapiMic(const WasapiMic&) = delete;
    WasapiMic& operator=(const WasapiMic&) = delete;

    // Empty id = system default communications-capture device.
    bool start(const std::wstring& device_id);
    void stop();
    bool running() const { return started_; }

    uint32_t read(float* dst, uint32_t max_frames);
    uint32_t sample_rate() const { return sample_rate_; }
    uint16_t channels() const { return channels_; }

private:
    void* client_ = nullptr;    // IAudioClient*
    void* capture_ = nullptr;   // IAudioCaptureClient*
    uint32_t sample_rate_ = 48000;
    uint16_t channels_ = 2;
    uint16_t bits_per_sample_ = 32;
    bool float_format_ = true;
    bool started_ = false;
};

// Live level preview for the mic picker: opens tiny shared-mode capture
// streams on each requested device and reports peak % ~8x/second while alive.
class MicPreview {
public:
    ~MicPreview();
    static std::unique_ptr<MicPreview> start(
        const std::vector<std::wstring>& device_ids,
        std::function<void(const std::wstring& /*id*/, int /*pct*/)> on_level);

private:
    MicPreview() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cliplite::audio
