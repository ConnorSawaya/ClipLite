#include "cliplite/library/text_overlay.h"

#include <algorithm>

#include "cliplite/log.h"

namespace cliplite::library {

namespace {
// BT.601 limited-range, matching frame_converter.cpp rounding.
inline uint8_t rgb_to_y(int r, int g, int b) {
    const int v = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    return static_cast<uint8_t>(v < 16 ? 16 : (v > 235 ? 235 : v));
}
inline uint8_t rgb_to_u(int r, int g, int b) {
    const int v = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    return static_cast<uint8_t>(v < 16 ? 16 : (v > 240 ? 240 : v));
}
inline uint8_t rgb_to_v(int r, int g, int b) {
    const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    return static_cast<uint8_t>(v < 16 ? 16 : (v > 240 ? 240 : v));
}
}  // namespace

bool prerender_text_bitmap(const EditText& t, int cap_px, TextBitmap& out) {
    out = TextBitmap{};
    if (t.text.empty() || t.text.size() > 200 || cap_px <= 0) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) return false;

    HFONT font = CreateFontW(-cap_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             t.family.c_str());
    bool ok = false;
    if (font) {
        HGDIOBJ old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        // Measure first (single line, no prefix processing).
        RECT rc{0, 0, 0, 0};
        DrawTextW(dc, t.text.c_str(), static_cast<int>(t.text.size()), &rc,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w > 0 && h > 0 && w <= 4096 && h <= 1024) {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;  // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp && bits) {
                HGDIOBJ old_bmp = SelectObject(dc, bmp);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 255));
                // Black background, white glyphs: red channel == coverage.
                RECT fill{0, 0, w, h};
                HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                FillRect(dc, &fill, black);
                DrawTextW(dc, t.text.c_str(), static_cast<int>(t.text.size()), &fill,
                          DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
                GdiFlush();
                out.w = w;
                out.h = h;
                out.coverage.resize(static_cast<size_t>(w) * h);
                const uint8_t* px = static_cast<const uint8_t*>(bits);
                // Top-down DIB: row y at y*w*4, red channel first (BGRA).
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        out.coverage[static_cast<size_t>(y) * w + x] =
                            px[static_cast<size_t>(y) * w * 4 + x * 4 + 2];
                    }
                }
                out.y = rgb_to_y(t.r, t.g, t.b);
                out.u = rgb_to_u(t.r, t.g, t.b);
                out.v = rgb_to_v(t.r, t.g, t.b);
                SelectObject(dc, old_bmp);
                DeleteObject(bmp);
                ok = out.w > 0;
            }
        }
        SelectObject(dc, old_font);
        DeleteObject(font);
    }
    DeleteDC(dc);
    if (!ok) CL_WARN("Text", "prerender failed, skipping overlay");
    return ok;
}

void blend_text_nv12(uint8_t* y_plane, uint8_t* uv_plane, int W, int H,
                     const TextBitmap& tb, int x0, int y0) {
    if (!y_plane || !uv_plane || W <= 0 || H <= 0 || tb.w <= 0 || tb.h <= 0) return;
    for (int ty = 0; ty < tb.h; ++ty) {
        const int dy = y0 + ty;
        if (dy < 0 || dy >= H) continue;
        for (int tx = 0; tx < tb.w; ++tx) {
            const int dx = x0 + tx;
            if (dx < 0 || dx >= W) continue;
            const uint8_t cov =
                tb.coverage[static_cast<size_t>(ty) * tb.w + tx];
            if (cov == 0) continue;
            uint8_t& dst = y_plane[static_cast<size_t>(dy) * W + dx];
            dst = static_cast<uint8_t>((dst * (255 - cov) + tb.y * cov + 127) / 255);
            // Chroma is shared per 2x2 block: blend once at even origins to
            // avoid four competing writes per block.
            if ((dx & 1) == 0 && (dy & 1) == 0) {
                const int ux = dx / 2, uy = dy / 2;
                uint8_t& du = uv_plane[static_cast<size_t>(uy) * W + ux * 2];
                uint8_t& dv = uv_plane[static_cast<size_t>(uy) * W + ux * 2 + 1];
                du = static_cast<uint8_t>((du * (255 - cov) + tb.u * cov + 127) / 255);
                dv = static_cast<uint8_t>((dv * (255 - cov) + tb.v * cov + 127) / 255);
            }
        }
    }
}

void text_top_left(float x, float y, TextAlign align, int W, int H, int tw, int th,
                   int& x0, int& y0) {
    int ax = static_cast<int>(x * W);
    int ay = static_cast<int>(y * H);
    if (align == TextAlign::Center) ax -= tw / 2;
    if (align == TextAlign::Right) ax -= tw;
    ay -= th / 2;
    x0 = ax;
    y0 = ay;
}

}  // namespace cliplite::library
