#include "cliplite/graphics/frame_converter.h"

namespace cliplite::graphics {

namespace {
inline uint8_t clamp255(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}
}  // namespace

namespace {
// Clamp-to-edge bilinear sample of a single-byte plane (sw/sh >= 2).
inline uint8_t bilinear(const uint8_t* p, uint32_t sw, uint32_t sh, float x, float y) {
    int x0 = static_cast<int>(x);
    int y0 = static_cast<int>(y);
    if (x0 < 0) x0 = 0;
    if (static_cast<uint32_t>(x0) >= sw - 1) x0 = static_cast<int>(sw) - 2;
    if (y0 < 0) y0 = 0;
    if (static_cast<uint32_t>(y0) >= sh - 1) y0 = static_cast<int>(sh) - 2;
    float fx = x - static_cast<float>(x0);
    float fy = y - static_cast<float>(y0);
    if (fx < 0.f) fx = 0.f;
    if (fx > 1.f) fx = 1.f;
    if (fy < 0.f) fy = 0.f;
    if (fy > 1.f) fy = 1.f;
    const float a = p[static_cast<size_t>(y0) * sw + x0];
    const float b = p[static_cast<size_t>(y0) * sw + x0 + 1];
    const float c = p[static_cast<size_t>(y0 + 1) * sw + x0];
    const float d = p[static_cast<size_t>(y0 + 1) * sw + x0 + 1];
    return static_cast<uint8_t>(a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy);
}
}  // namespace

void nv12_scale(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst,
                uint32_t dw, uint32_t dh) {
    // Even >= 2 dimensions only (NV12 chroma pairing); anything else is a
    // caller bug — refuse rather than reading out of bounds.
    if (!src || !dst) return;
    if (sw < 2 || sh < 2 || dw < 2 || dh < 2) return;
    if ((sw | sh | dw | dh) & 1u) return;
    // Luma.
    for (uint32_t y = 0; y < dh; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) * sh / dh - 0.5f;
        for (uint32_t x = 0; x < dw; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * sw / dw - 0.5f;
            dst[static_cast<size_t>(y) * dw + x] = bilinear(src, sw, sh, sx, sy);
        }
    }
    // Chroma: interleaved pairs form a (sw/2 x sh/2) grid of 2-byte texels.
    const uint32_t sww2 = sw / 2, shh2 = sh / 2, dww2 = dw / 2, dhh2 = dh / 2;
    const uint8_t* suv = src + static_cast<size_t>(sw) * sh;
    uint8_t* duv = dst + static_cast<size_t>(dw) * dh;
    for (uint32_t y = 0; y < dhh2; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) * shh2 / dhh2 - 0.5f;
        for (uint32_t x = 0; x < dww2; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * sww2 / dww2 - 0.5f;
            int x0 = static_cast<int>(sx);
            int y0 = static_cast<int>(sy);
            if (x0 < 0) x0 = 0;
            if (static_cast<uint32_t>(x0) >= sww2 - 1) x0 = static_cast<int>(sww2) - 2;
            if (y0 < 0) y0 = 0;
            if (static_cast<uint32_t>(y0) >= shh2 - 1) y0 = static_cast<int>(shh2) - 2;
            float fx = sx - x0;
            float fy = sy - y0;
            if (fx < 0.f) fx = 0.f;
            if (fx > 1.f) fx = 1.f;
            if (fy < 0.f) fy = 0.f;
            if (fy > 1.f) fy = 1.f;
            for (int k = 0; k < 2; ++k) {
                const size_t r0 = (static_cast<size_t>(y0) * sww2 + x0) * 2 + k;
                const float a = suv[r0];
                const float b = suv[r0 + 2];
                const float c = suv[r0 + static_cast<size_t>(sww2) * 2];
                const float d = suv[r0 + static_cast<size_t>(sww2) * 2 + 2];
                duv[(static_cast<size_t>(y) * dww2 + x) * 2 + k] =
                    static_cast<uint8_t>(a + (b - a) * fx + (c - a) * fy +
                                         (a - b - c + d) * fx * fy);
            }
        }
    }
}

void bgra_to_nv12(const uint8_t* bgra, uint32_t src_stride, uint32_t width, uint32_t height,
                  uint8_t* nv12_out) {
    uint8_t* y = nv12_out;
    uint8_t* uv = nv12_out + static_cast<size_t>(width) * height;

    // Luma (BT.601 limited range).
    for (uint32_t j = 0; j < height; ++j) {
        const uint8_t* row = bgra + static_cast<size_t>(j) * src_stride;
        uint8_t* yrow = y + static_cast<size_t>(j) * width;
        for (uint32_t i = 0; i < width; ++i) {
            const int b = row[i * 4 + 0];
            const int g = row[i * 4 + 1];
            const int r = row[i * 4 + 2];
            const int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yrow[i] = clamp255(Y);
        }
    }

    // Chroma (subsampled 2x2 average).
    for (uint32_t j = 0; j < height; j += 2) {
        const uint8_t* row0 = bgra + static_cast<size_t>(j) * src_stride;
        const uint8_t* row1 = (j + 1 < height) ? bgra + static_cast<size_t>(j + 1) * src_stride : row0;
        uint8_t* uvrow = uv + static_cast<size_t>(j / 2) * width;
        for (uint32_t i = 0; i < width; i += 2) {
            const uint32_t i1 = (i + 1 < width) ? i + 1 : i;
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const uint8_t* row = dy ? row1 : row0;
                for (int dx = 0; dx < 2; ++dx) {
                    const uint32_t idx = (dx ? i1 : i) * 4;
                    b += row[idx + 0];
                    g += row[idx + 1];
                    r += row[idx + 2];
                }
            }
            r /= 4;
            g /= 4;
            b /= 4;
            const int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            const int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            uvrow[i] = clamp255(U);
            uvrow[i + 1] = clamp255(V);
        }
    }
}

}  // namespace cliplite::graphics
