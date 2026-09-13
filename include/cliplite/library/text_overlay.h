#pragma once

// Basic text overlays for the built-in editor: single-line GDI-rasterized
// text blended onto NV12 frames. Deliberately small (no layout engine):
// content, size, color, anchor position, alignment, time range.
//
// Callers prerender each text once per render (output-size dependent), then
// blend per emitted frame whose output timestamp falls in range.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cliplite::library {

// Left/center/right anchor: which point of the text box (x,y) positions.
enum class TextAlign { Left, Center, Right };

struct EditText {
    std::wstring text;              // single line, non-empty, <= 200 chars
    std::wstring family = L"Segoe UI";
    float size_frac = 0.09f;        // cap-height fraction of frame height
    uint8_t r = 255, g = 255, b = 255;
    float x = 0.5f, y = 0.85f;      // anchor point, fractions of frame
    TextAlign align = TextAlign::Center;
    int64_t start_ms = 0;           // inclusive, output timeline
    int64_t end_ms = 0;             // exclusive, output timeline (0 = unused)
};

// Rasterized coverage mask + resolved color. w/h > 0 when usable.
struct TextBitmap {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> coverage;  // w*h, 0..255 (white-on-black luminance)
    uint8_t y = 235, u = 128, v = 128;  // text color in limited-range NV12
};

// Rasterizes text at cap-height px for the given family. Returns false when
// there is nothing drawable (empty text, bad font, GDI failure); the caller
// skips such texts with a warning instead of failing the render.
bool prerender_text_bitmap(const EditText& t, int cap_px, TextBitmap& out);

// Blends a prerendered text onto tight NV12 (W*H) at top-left (x0, y0).
// Out-of-frame pixels are clipped, never written.
void blend_text_nv12(uint8_t* y_plane, uint8_t* uv_plane, int W, int H,
                     const TextBitmap& tb, int x0, int y0);

// Anchor point (x,y fractions) + alignment -> top-left pixel for a bitmap.
void text_top_left(float x, float y, TextAlign align, int W, int H, int tw, int th,
                   int& x0, int& y0);

}  // namespace cliplite::library
