#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>

namespace cliplite::capture {

struct CapturedFrame {
    const uint8_t* bgra = nullptr;  // valid until release_frame()
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;            // bytes per row
};

// Captures an entire monitor via DXGI Desktop Duplication (the spec's
// whole-display capture path, alternative/fallback to Windows Graphics Capture).
// Frames are copied to a CPU-readable BGRA staging buffer.
class DxgiDesktopDuplication {
public:
    DxgiDesktopDuplication() = default;
    ~DxgiDesktopDuplication();
    DxgiDesktopDuplication(const DxgiDesktopDuplication&) = delete;
    DxgiDesktopDuplication& operator=(const DxgiDesktopDuplication&) = delete;

    bool start(uint32_t output_index = 0);
    void stop();
    bool running() const { return dup_ != nullptr; }

    // Acquires the next frame. Returns false on timeout or error. On success,
    // `out` points into an internal buffer valid until release_frame().
    bool acquire_frame(CapturedFrame* out, uint32_t timeout_ms = 1000);
    void release_frame();

    // True after desktop duplication became unusable (DXGI_ERROR_ACCESS_LOST,
    // DXGI_ERROR_INVALID_CALL, driver reset, ...). The caller should recreate
    // the capture via stop() + start().
    bool lost() const { return lost_; }

    uint32_t width() const { return desc_.ModeDesc.Width; }
    uint32_t height() const { return desc_.ModeDesc.Height; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    DXGI_OUTDUPL_DESC desc_{};
    bool frame_acquired_ = false;
    bool mapped_ = false;
    bool lost_ = false;
    bool failure_logged_ = false;  // log each failure streak once, not every retry
};

}  // namespace cliplite::capture
