#include "cliplite/audio/process_loopback.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>

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

// Completion handler for ActivateAudioInterfaceAsync. Implements IAgileObject
// so activation can complete on any apartment without E_ILLEGAL_METHOD_CALL.
class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivateHandler() : refs_(1), done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *pp = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        if (op) (void)op->GetActivateResult(&hr_, &result_);
        SetEvent(done_);
        return S_OK;
    }

    HANDLE done() const { return done_; }
    HRESULT result_hr() const { return hr_; }
    // Transfers ownership of the activation result (AddRef'd by the system, or
    // nullptr on failure). Caller must Attach without AddRef.
    IUnknown* detach_result() {
        IUnknown* p = result_;
        result_ = nullptr;
        return p;
    }

private:
    ~ActivateHandler() {
        if (result_) result_->Release();
        if (done_) CloseHandle(done_);
    }
    volatile LONG refs_ = 0;
    HANDLE done_ = nullptr;
    HRESULT hr_ = E_FAIL;
    IUnknown* result_ = nullptr;
};

// Message-pumping wait so STA callers do not deadlock when completion is
// marshaled back to this thread. Returns true when signaled.
bool pump_wait(HANDLE h, DWORD timeout_ms) {
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        const ULONGLONG elapsed = GetTickCount64() - t0;
        if (elapsed >= timeout_ms) return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        const DWORD left = static_cast<DWORD>(timeout_ms - elapsed);
        const DWORD w = MsgWaitForMultipleObjects(1, &h, FALSE, left, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) return true;
        if (w == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        return false;  // WAIT_FAILED / WAIT_TIMEOUT
    }
}
}  // namespace

ProcessLoopbackCapture::~ProcessLoopbackCapture() {
    stop();
}

bool ProcessLoopbackCapture::start(uint32_t target_pid) {
    if (started_) return false;
    if (target_pid == 0) {
        CL_ERROR("Audio", "process loopback: pid 0 has no capturable tree");
        return false;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = target_pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT prop{};
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    Microsoft::WRL::ComPtr<IActivateAudioInterfaceCompletionHandler> handler;
    handler.Attach(new (std::nothrow) ActivateHandler());
    if (!handler) {
        CL_ERROR("Audio", "process loopback: handler alloc failed");
        return false;
    }
    // Keep a raw pointer: ComPtr owns our creation ref; the async operation
    // holds its own ref until completion.
    auto* raw = static_cast<ActivateHandler*>(handler.Get());

    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &prop, handler.Get(), &op);
    if (FAILED(hr)) {
        CL_ERROR("Audio", "process loopback: ActivateAudioInterfaceAsync failed (hr=" +
                              hr_hex(hr) + "); OS may predate process loopback");
        return false;
    }

    if (!pump_wait(raw->done(), 10000)) {
        CL_ERROR("Audio", "process loopback: activation timed out");
        return false;
    }
    if (FAILED(raw->result_hr())) {
        CL_ERROR("Audio", "process loopback: activation result failed (hr=" +
                              hr_hex(raw->result_hr()) + ")");
        return false;
    }

    Microsoft::WRL::ComPtr<IUnknown> unk;
    unk.Attach(raw->detach_result());
    if (!unk) {
        CL_ERROR("Audio", "process loopback: empty activation result");
        return false;
    }
    hr = unk.As(&client_);
    if (FAILED(hr) || !client_) {
        CL_ERROR("Audio", "process loopback: QI IAudioClient failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    // GetMixFormat is E_NOTIMPL for process loopback: request fixed 48kHz
    // float stereo and let the engine convert (AUTOCONVERTPCM).
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 2 * 4;
    fmt.nAvgBytesPerSec = 48000 * 2 * 4;
    fmt.cbSize = 0;

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                             10000000 / 10, 0, &fmt, nullptr);
    if (FAILED(hr)) {
        CL_ERROR("Audio", "process loopback: Initialize failed (hr=" + hr_hex(hr) + ")");
        client_.Reset();
        return false;
    }

    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) {
        CL_ERROR("Audio", "process loopback: IAudioCaptureClient failed (hr=" + hr_hex(hr) + ")");
        client_.Reset();
        return false;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
        CL_ERROR("Audio", "process loopback: Start failed (hr=" + hr_hex(hr) + ")");
        capture_.Reset();
        client_.Reset();
        return false;
    }

    target_pid_ = target_pid;
    started_ = true;
    CL_INFO("Audio", "process loopback started (pid=" + std::to_string(target_pid) + ")");
    return true;
}

void ProcessLoopbackCapture::stop() {
    if (client_) client_->Stop();
    capture_.Reset();
    client_.Reset();
    target_pid_ = 0;
    started_ = false;
}

uint32_t ProcessLoopbackCapture::read(float* dst, uint32_t max_frames) {
    if (!started_ || !capture_ || !dst) return 0;
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
            std::memcpy(dst + static_cast<size_t>(total) * 2, data,
                        static_cast<size_t>(take) * 2 * sizeof(float));
            total += take;
        }
        capture_->ReleaseBuffer(frames);
    }
    return total;
}

}  // namespace cliplite::audio
