#pragma once

#include <cstdint>

namespace cliplite::graphics {

// Converts a BGRA8 frame (src_stride bytes per row) into tightly-packed NV12
// (Y plane of width*height followed by interleaved UV of width*height/2).
// Used to feed the H.264 encoder from DXGI Desktop Duplication (BGRA) frames.
void bgra_to_nv12(const uint8_t* bgra, uint32_t src_stride, uint32_t width, uint32_t height,
                  uint8_t* nv12_out);

// Bilinear scale of tightly-packed NV12 (src sw*sh -> dst dw*dh), either
// direction. All dimensions must be even and >= 2. Used for export resolution
// targets (1080p/720p) and aspect-canvas fit. Out-of-range inputs are clamped,
// never read.
void nv12_scale(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst,
                uint32_t dw, uint32_t dh);
// Legacy alias (downscale-only callers kept working after generalization).
inline void nv12_scale_down(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst,
                            uint32_t dw, uint32_t dh) {
    if (dw > sw) dw = sw;
    if (dh > sh) dh = sh;
    nv12_scale(src, sw, sh, dst, dw, dh);
}

}  // namespace cliplite::graphics
