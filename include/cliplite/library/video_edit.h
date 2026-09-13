#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cliplite/library/text_overlay.h"

namespace cliplite::library {

// A rectangle to blur, in fractions of the (post-crop) frame: 0..1.
struct EditBlur {
    float x = 0;
    float y = 0;
    float w = 0.2f;
    float h = 0.15f;
};

struct EditOptions {
    int64_t start_ms = 0;      // inclusive; 0 = from beginning
    int64_t end_ms = 0;        // exclusive; 0 = to end
    int crop_x = 0;            // pixels in source coordinates; crop_w/h == 0 disables
    int crop_y = 0;
    int crop_w = 0;
    int crop_h = 0;
    std::vector<EditBlur> blurs;
    // Optional pre-mixed stem audio (48kHz stereo float) covering exactly
    // [start_ms, end_ms) of the input timeline. Non-owning: the caller keeps
    // it alive for the whole render (e.g. a job member). When set and
    // non-empty it is AAC-encoded instead of the original-audio passthrough,
    // unifying trim/crop/blur/app-exclusion in one pass.
    const std::vector<float>* mix_pcm = nullptr;
    // Output-timeline fades (edit_project_video only; edit_video ignores).
    // Video fades to/from black, audio fades are pre-applied to mix_pcm.
    int64_t fade_in_ms = 0;
    int64_t fade_out_ms = 0;
    // Export targets (edit_project_video only). out_height 0 = keep crop
    // size, else downscale to that height preserving aspect (even dims).
    // out_fps 0 = source rate, else 30/60 output pacing. quality 0 low,
    // 1 medium (default), 2 high: bitrate multiplier on the area formula.
    int out_height = 0;
    int out_fps = 0;
    int quality = 1;
    // Text overlays (edit_project_video only): blended after scale, under
    // fades, each within its output-timeline range.
    std::vector<EditText> texts;
    // Force video-only output even when a passthrough/copy path exists
    // (all-muted renders: silence requested, not original audio).
    bool force_video_only = false;
    // Aspect canvas (edit_project_video only): 0 = off (fill behavior —
    // aspect presets center-crop). When set, the source window is FIT
    // (scaled whole + black bars) into the aspect box instead of cropped.
    int canvas_ar_w = 0;
    int canvas_ar_h = 0;
};

// Simple video editor: decodes the input (NV12), applies an optional crop and
// box-blur regions over the selected time range, re-encodes H.264 via Media
// Foundation (hardware when available) and copies the original audio through
// untouched — unless opts.mix_pcm is set, in which case that mix is encoded
// instead. Progress is reported as 0..100; returning false from `progress`
// cancels and fails the render.
bool edit_video(const std::wstring& input, const std::wstring& output, const EditOptions& opts,
                const std::function<bool(int)>& progress);

// One timeline segment for edit_project_video(): a source window of one input
// file. Segments may reference different files (imported clips); each is
// decoded with its own reader, crop-clamped into its own dimensions, and
// normalized to the project output size via the export scaler.
struct ProjectSegment {
    std::wstring input;
    int64_t start_ms = 0;  // inclusive, source time
    int64_t end_ms = 0;    // exclusive, source time
    float volume = 1.0f;   // applied to mix_pcm slices (1.0 = unchanged)
    double speed = 1.0;    // 0.25..4.0; output dur = source span / speed
};

// Multi-segment timeline render in ONE encoder session: each segment is
// decoded from its source window (same crop/blur applied) and concatenated on
// the output timeline. mix_pcm, when set, must cover the CONCATENATED timeline
// from 0 (callers slice per-segment source audio, resample by speed, and
// concatenate first). Progress spans the whole timeline; cancel/cleanup
// matches edit_video.
bool edit_project_video(const std::vector<ProjectSegment>& segments, const std::wstring& output,
                        const EditOptions& opts,
                        const std::function<bool(int)>& progress);

}  // namespace cliplite::library
