#include "cliplite/audio/wasapi_mic.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "cliplite/log.h"
#include "cliplite/util/win_utf8.h"

namespace cliplite::audio {

namespace {

using Microsoft::WRL::ComPtr;

std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

std::vector<MicDeviceInfo> enumerate_mics() {
    std::vector<MicDeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        CL_ERROR("Mic", "MMDeviceEnumerator failed (hr=" + hr_hex(hr) + ")");
        return out;
    }
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        CL_ERROR("Mic", "EnumAudioEndpoints failed (hr=" + hr_hex(hr) + ")");
        return out;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        MicDeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var))) {
                info.name = cliplite::util::wide_to_utf8(var.pwszVal ? var.pwszVal : L"");
                PropVariantClear(&var);
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::wstring default_mic_id() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return {};
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) return {};
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) return {};
    std::wstring out(id);
    CoTaskMemFree(id);
    return out;
}

// ---------------------------------------------------------------------------
// WasapiMic
// ---------------------------------------------------------------------------

WasapiMic::~WasapiMic() { stop(); }

bool WasapiMic::start(const std::wstring& device_id) {
    if (started_) stop();

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        CL_ERROR("Mic", "MMDeviceEnumerator failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    ComPtr<IMMDevice> device;
    if (device_id.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    } else {
        hr = enumerator->GetDevice(device_id.c_str(), &device);
    }
    if (FAILED(hr)) {
        CL_ERROR("Mic", "capture device open failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) {
        CL_ERROR("Mic", "IAudioClient activate failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    WAVEFORMATEX* fmt = nullptr;
    hr = client->GetMixFormat(&fmt);
    if (FAILED(hr) || !fmt) {
        CL_ERROR("Mic", "GetMixFormat failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    sample_rate_ = fmt->nSamplesPerSec;
    channels_ = fmt->nChannels;
    bits_per_sample_ = fmt->wBitsPerSample;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        float_format_ = (wfe->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        float_format_ = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    }

    const REFERENCE_TIME buffer_duration = 10000000 / 10;  // 100 ms
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buffer_duration, 0, fmt, nullptr);
    if (FAILED(hr)) {
        CL_ERROR("Mic", "Initialize failed (hr=" + hr_hex(hr) + ")");
        CoTaskMemFree(fmt);
        return false;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) {
        CL_ERROR("Mic", "IAudioCaptureClient failed (hr=" + hr_hex(hr) + ")");
        CoTaskMemFree(fmt);
        return false;
    }

    hr = client->Start();
    CoTaskMemFree(fmt);
    if (FAILED(hr)) {
        CL_ERROR("Mic", "Start failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    client_ = client.Detach();
    capture_ = capture.Detach();
    started_ = true;
    CL_INFO("Mic", "mic started (" + std::to_string(sample_rate_) + " Hz, " +
                       std::to_string(channels_) + " ch)");
    return true;
}

void WasapiMic::stop() {
    if (client_) static_cast<IAudioClient*>(client_)->Stop();
    if (capture_) static_cast<IAudioCaptureClient*>(capture_)->Release();
    if (client_) static_cast<IAudioClient*>(client_)->Release();
    capture_ = nullptr;
    client_ = nullptr;
    started_ = false;
}

uint32_t WasapiMic::read(float* dst, uint32_t max_frames) {
    if (!started_ || !capture_) return 0;
    auto* cap = static_cast<IAudioCaptureClient*>(capture_);
    uint32_t total = 0;
    while (total < max_frames) {
        uint32_t packet = 0;
        if (FAILED(cap->GetNextPacketSize(&packet)) || packet == 0) break;
        BYTE* data = nullptr;
        uint32_t frames = 0;
        DWORD flags = 0;
        if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (frames > 0 && data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            const uint32_t take = (frames > max_frames - total) ? (max_frames - total) : frames;
            if (float_format_) {
                std::memcpy(dst + static_cast<size_t>(total) * channels_, data,
                            static_cast<size_t>(take) * channels_ * sizeof(float));
            } else if (bits_per_sample_ == 16) {
                const int16_t* src = reinterpret_cast<const int16_t*>(data);
                for (uint32_t i = 0; i < take * channels_; ++i)
                    dst[static_cast<size_t>(total) * channels_ + i] = src[i] / 32768.0f;
            } else {
                const int32_t* src = reinterpret_cast<const int32_t*>(data);
                for (uint32_t i = 0; i < take * channels_; ++i)
                    dst[static_cast<size_t>(total) * channels_ + i] = src[i] / 2147483648.0f;
            }
            total += take;
        }
        cap->ReleaseBuffer(frames);
        if (frames == 0) break;
    }
    return total;
}

// ---------------------------------------------------------------------------
// MicPreview
// ---------------------------------------------------------------------------

struct MicPreview::Impl {
    std::vector<std::wstring> ids;
    std::function<void(const std::wstring&, int)> on_level;
    std::atomic<bool> run{true};
    std::thread th;

    void loop() {
        struct Stream {
            std::wstring id;
            WasapiMic mic;
        };
        std::vector<std::unique_ptr<Stream>> streams;
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_ok = SUCCEEDED(co);
        for (const auto& id : ids) {
            auto s = std::make_unique<Stream>();
            s->id = id;
            if (s->mic.start(id)) streams.push_back(std::move(s));
        }
        std::vector<float> scratch(48000 / 10 * 8);  // 100ms @ up to 48k x8ch headroom
        while (run.load()) {
            for (const auto& s : streams) {
                const uint32_t n =
                    s->mic.read(scratch.data(), static_cast<uint32_t>(scratch.size()) /
                                                    std::max<uint16_t>(1u, s->mic.channels()));
                float peak = 0.f;
                const uint16_t ch = std::max<uint16_t>(1u, s->mic.channels());
                for (uint32_t i = 0; i < n * ch; ++i) peak = std::max(peak, scratch[i] < 0 ? -scratch[i] : scratch[i]);
                int pct = static_cast<int>(peak * 140);
                if (pct > 100) pct = 100;
                if (on_level) on_level(s->id, pct);
            }
            Sleep(120);
        }
        streams.clear();
        if (com_ok) CoUninitialize();
    }
};

MicPreview::~MicPreview() {
    if (impl_) {
        impl_->run = false;
        if (impl_->th.joinable()) impl_->th.join();
    }
}

std::unique_ptr<MicPreview> MicPreview::start(
    const std::vector<std::wstring>& device_ids,
    std::function<void(const std::wstring&, int)> on_level) {
    auto m = std::unique_ptr<MicPreview>(new MicPreview());
    m->impl_ = std::make_unique<Impl>();
    m->impl_->ids = device_ids;
    m->impl_->on_level = std::move(on_level);
    m->impl_->th = std::thread([p = m->impl_.get()] { p->loop(); });
    return m;
}

}  // namespace cliplite::audio
