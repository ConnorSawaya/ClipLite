#pragma once

// Lightweight nondestructive timeline model for the built-in editor.
//
// Adapted from the editing model pioneered by OpenCut Classic (MIT licensed,
// https://github.com/OpenCut-app/opencut-classic — timeline concepts such as
// source-window clips, ripple layout, and command-based history; see
// THIRD_PARTY_NOTICES.md). This implementation is original code written for
// ClipLite's C++ core: integer-millisecond time, single video track, pure
// functions with no Win32/Media Foundation dependency so it unit-tests
// anywhere.
//
// Model: a Project references media Sources by id and orders Clips on one
// track. Each clip views a [sourceStart, sourceEnd) window of its source;
// ripple() derives every timelineStart so the track is gapless. All mutating
// ops re-ripple. Editing never touches source files; only export renders.

#include <cstdint>
#include <string>
#include <vector>

namespace cliplite::timeline {

constexpr int64_t kMinClipMs = 100;     // slivers below this are rejected
constexpr int64_t kSplitMarginMs = 50;  // split must clear both edges by this
constexpr double kMinSpeed = 0.25;
constexpr double kMaxSpeed = 4.0;
constexpr float kMaxVolume = 2.0f;

struct TimelineSource {
    std::string id;
    int64_t duration_ms = 0;  // probed source length, > 0
};

struct TimelineClip {
    std::string id;
    std::string source_id;
    int64_t sourceStart_ms = 0;   // inclusive, in source time
    int64_t sourceEnd_ms = 0;     // exclusive, in source time
    int64_t timelineStart_ms = 0;  // derived by ripple(), never hand-edited
    double speed = 1.0;            // reserved (render MVP requires 1.0)
    float volume = 1.0f;           // per-clip linear gain, 0..2
    bool muted = false;

    int64_t sourceDur_ms() const { return sourceEnd_ms - sourceStart_ms; }
    int64_t timelineDur_ms() const;  // speed-adjusted, >= 1 when valid
};

struct TimelineProject {
    std::vector<TimelineSource> sources;  // MVP: exactly 1
    std::vector<TimelineClip> clips;      // ordered, gapless after ripple()

    int64_t duration_ms() const;
    const TimelineSource* find_source(const std::string& id) const;
};

// Structural check. When err is non-null it receives a short reason.
bool validate(const TimelineProject& p, std::string* err = nullptr);

// Recompute every timelineStart from 0 (gapless). Call after any mutation.
void ripple(TimelineProject& p);

// Split clip_id at source time t_ms: left keeps [s,t), right [t,e) with
// new_clip_id. No-op (false) when t is within kSplitMarginMs of either edge.
bool split(TimelineProject& p, const std::string& clip_id, int64_t t_ms,
           const std::string& new_clip_id);

// Move one timeline edge by delta_ms (edge -1 = left/sourceStart,
// +1 = right/sourceEnd), clamped to source bounds and kMinClipMs.
bool trim(TimelineProject& p, const std::string& clip_id, int edge, int64_t delta_ms);

// Erase a clip and ripple the gap closed. False when id is unknown.
bool remove(TimelineProject& p, const std::string& clip_id);

// Reorder a clip to to_index (clamped; negatives go front) and re-ripple.
// False when unknown.
bool move(TimelineProject& p, const std::string& clip_id, int64_t to_index);

}  // namespace cliplite::timeline
