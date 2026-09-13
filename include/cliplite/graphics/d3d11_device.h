#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace cliplite::graphics {

// RAII D3D11 device + immediate context with BGRA support (required by
// Windows Graphics Capture). Falls back to the WARP software rasterizer if no
// hardware adapter is available.
class D3D11Device {
public:
    bool create(D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_HARDWARE);

    ID3D11Device* get() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    D3D_FEATURE_LEVEL feature_level() const { return level_; }
    explicit operator bool() const { return device_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    D3D_FEATURE_LEVEL level_ = D3D_FEATURE_LEVEL_11_0;
};

}  // namespace cliplite::graphics
