#include "cliplite/capture/dxgi_capture.h"

#include <cstdio>

#include "cliplite/log.h"

namespace cliplite::capture {

namespace {
std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}
}  // namespace

DxgiDesktopDuplication::~DxgiDesktopDuplication() {
    stop();
}

bool DxgiDesktopDuplication::start(uint32_t output_index) {
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr,
                                   context_.GetAddressOf());
    if (FAILED(hr)) {
        CL_ERROR("Capture", "D3D11 device failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = device_.As(&dxgi_device);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(output_index, &output);
    if (FAILED(hr)) {
        CL_ERROR("Capture", "no output at index " + std::to_string(output_index));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) return false;

    hr = output1->DuplicateOutput(device_.Get(), &dup_);
    if (FAILED(hr)) {
        CL_ERROR("Capture", "DuplicateOutput failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    dup_->GetDesc(&desc_);
    lost_ = false;
    failure_logged_ = false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc_.ModeDesc.Width;
    td.Height = desc_.ModeDesc.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = desc_.ModeDesc.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&td, nullptr, &staging_);
    if (FAILED(hr)) {
        CL_ERROR("Capture", "staging texture failed (hr=" + hr_hex(hr) + ")");
        return false;
    }

    CL_INFO("Capture", "desktop duplication started " + std::to_string(desc_.ModeDesc.Width) +
                           "x" + std::to_string(desc_.ModeDesc.Height));
    return true;
}

void DxgiDesktopDuplication::stop() {
    release_frame();
    dup_.Reset();
    staging_.Reset();
    context_.Reset();
    device_.Reset();
}

bool DxgiDesktopDuplication::acquire_frame(CapturedFrame* out, uint32_t timeout_ms) {
    if (!dup_) return false;
    if (mapped_) release_frame();

    DXGI_OUTDUPL_FRAME_INFO info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = dup_->AcquireNextFrame(timeout_ms, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        lost_ = true;
        CL_WARN("Capture", "desktop access lost (screen locked / session change)");
        return false;
    }
    if (FAILED(hr)) {
        // Any other persistent failure (e.g. DXGI_ERROR_INVALID_CALL after a
        // mode change or driver reset) leaves the duplication unusable. Mark it
        // lost so the caller recreates it instead of hammering a dead object
        // and flooding the log at frame rate.
        lost_ = true;
        if (!failure_logged_) {
            failure_logged_ = true;
            CL_ERROR("Capture", "AcquireNextFrame failed (hr=" + hr_hex(hr) +
                                    "); recreating desktop duplication");
        }
        return false;
    }
    failure_logged_ = false;
    frame_acquired_ = true;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    hr = resource.As(&tex);
    if (FAILED(hr)) {
        dup_->ReleaseFrame();
        frame_acquired_ = false;
        return false;
    }

    context_->CopyResource(staging_.Get(), tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        dup_->ReleaseFrame();
        frame_acquired_ = false;
        return false;
    }
    mapped_ = true;

    out->bgra = static_cast<const uint8_t*>(mapped.pData);
    out->stride = mapped.RowPitch;
    out->width = desc_.ModeDesc.Width;
    out->height = desc_.ModeDesc.Height;
    return true;
}

void DxgiDesktopDuplication::release_frame() {
    if (mapped_) {
        context_->Unmap(staging_.Get(), 0);
        mapped_ = false;
    }
    if (frame_acquired_) {
        dup_->ReleaseFrame();
        frame_acquired_ = false;
    }
}

}  // namespace cliplite::capture
