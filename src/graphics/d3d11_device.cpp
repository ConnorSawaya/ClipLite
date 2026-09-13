#include "cliplite/graphics/d3d11_device.h"

#include "cliplite/log.h"

namespace cliplite::graphics {

bool D3D11Device::create(D3D_DRIVER_TYPE driver) {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    const UINT level_count = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));

    HRESULT hr = D3D11CreateDevice(nullptr, driver, nullptr, flags, levels, level_count,
                                   D3D11_SDK_VERSION, device_.GetAddressOf(), &level_,
                                   context_.GetAddressOf());
    if (FAILED(hr) && driver == D3D_DRIVER_TYPE_HARDWARE) {
        CL_WARN("Graphics", "hardware D3D11 device failed, falling back to WARP");
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, level_count,
                               D3D11_SDK_VERSION, device_.GetAddressOf(), &level_,
                               context_.GetAddressOf());
    }
    if (FAILED(hr)) {
        CL_ERROR("Graphics", "D3D11 device creation failed");
        return false;
    }
    return true;
}

}  // namespace cliplite::graphics
