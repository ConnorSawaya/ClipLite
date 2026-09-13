#include "cliplite/audio/wasapi_loopback.h"

#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstring>
#include <cstdio>

#include "cliplite/log.h"

namespace cliplite::audio {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}
}  // namespace

WasapiLoopback::~WasapiLoopback() {
    stop();
}

bool WasapiLoopback::start(AudioFormat* out_format) {
    if (started_) return false;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        CL_ERROR("Audio", "MMDeviceEnumerator failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        CL_ERROR("Audio", "no default render device (hr=" + hr_hex(hr) + ")");
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) {
        CL_ERROR("Audio", "IAudioClient activate failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    hr = client_->GetMixFormat(&mix_format_);
    if (FAILED(hr) || !mix_format_) {
        CL_ERROR("Audio", "GetMixFormat failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    sample_rate_ = mix_format_->nSamplesPerSec;
    channels_ = mix_format_->nChannels;
    bits_per_sample_ = mix_format_->wBitsPerSample;

    if (mix_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_format_);
        float_format_ = (wfe->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        float_format_ = (mix_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    }
    if (!float_format_ && bits_per_sample_ != 16 && bits_per_sample_ != 32) {
        CL_WARN("Audio", "unexpected loopback sample format; treating as 16-bit PCM");
    }

    const REFERENCE_TIME buffer_duration = 10000000 / 10;  // 100 ms (10 Hz)
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                             buffer_duration, 0, mix_format_, nullptr);
    if (FAILED(hr)) {
        CL_ERROR("Audio", "loopback Initialize failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) {
        CL_ERROR("Audio", "IAudioCaptureClient failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
        CL_ERROR("Audio", "loopback Start failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    started_ = true;
    if (out_format) {
        out_format->sample_rate = sample_rate_;
        out_format->channels = channels_;
        out_format->bits_per_sample = bits_per_sample_;
    }
    CL_INFO("Audio", "loopback started (" + std::to_string(sample_rate_) + " Hz, " +
                         std::to_string(channels_) + " ch, " +
                         std::to_string(bits_per_sample_) + "-bit)");
    return true;
}

void WasapiLoopback::stop() {
    if (client_) client_->Stop();
    capture_.Reset();
    client_.Reset();
    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }
    started_ = false;
}

uint32_t WasapiLoopback::read(float* dst, uint32_t max_frames) {
    if (!started_ || !capture_) return 0;
    uint32_t total = 0;
    while (total < max_frames) {
        uint32_t packet = 0;
        HRESULT hr = capture_->GetNextPacketSize(&packet);
        if (FAILED(hr) || packet == 0) break;

        BYTE* data = nullptr;
        uint32_t frames = 0;
        DWORD flags = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;
        if (frames > 0 && data) {
            const uint32_t take = (frames > max_frames - total) ? (max_frames - total) : frames;
            if (float_format_) {
                std::memcpy(dst + static_cast<size_t>(total) * channels_, data,
                            static_cast<size_t>(take) * channels_ * sizeof(float));
            } else if (bits_per_sample_ == 16) {
                const int16_t* src = reinterpret_cast<const int16_t*>(data);
                for (uint32_t i = 0; i < take * channels_; ++i)
                    dst[static_cast<size_t>(total) * channels_ + i] =
                        src[i] / 32768.0f;
            } else {
                const int32_t* src = reinterpret_cast<const int32_t*>(data);
                for (uint32_t i = 0; i < take * channels_; ++i)
                    dst[static_cast<size_t>(total) * channels_ + i] =
                        src[i] / 2147483648.0f;
            }
            total += take;
        }
        capture_->ReleaseBuffer(frames);
        if (frames == 0) break;
    }
    return total;
}

}  // namespace cliplite::audio
