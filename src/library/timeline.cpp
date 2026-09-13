#include "cliplite/library/timeline.h"

#include <cmath>

namespace cliplite::timeline {

int64_t TimelineClip::timelineDur_ms() const {
    const int64_t src = sourceDur_ms();
    if (src <= 0 || speed <= 0.0) return 0;
    return static_cast<int64_t>(std::llround(static_cast<double>(src) / speed));
}

int64_t TimelineProject::duration_ms() const {
    int64_t total = 0;
    for (const auto& c : clips) total += c.timelineDur_ms();
    return total;
}

const TimelineSource* TimelineProject::find_source(const std::string& id) const {
    for (const auto& s : sources) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

bool validate(const TimelineProject& p, std::string* err) {
    auto fail = [&](const char* what) {
        if (err) *err = what;
        return false;
    };
    if (p.sources.empty()) return fail("no sources");
    for (const auto& s : p.sources) {
        if (s.id.empty()) return fail("source with empty id");
        if (s.duration_ms <= 0) return fail("source with non-positive duration");
    }
    int64_t expect_start = 0;
    for (size_t i = 0; i < p.clips.size(); ++i) {
        const auto& c = p.clips[i];
        if (c.id.empty()) return fail("clip with empty id");
        for (size_t j = 0; j < i; ++j) {
            if (p.clips[j].id == c.id) return fail("duplicate clip id");
        }
        const TimelineSource* src = p.find_source(c.source_id);
        if (!src) return fail("clip references unknown source");
        if (!(0 <= c.sourceStart_ms && c.sourceStart_ms < c.sourceEnd_ms &&
              c.sourceEnd_ms <= src->duration_ms))
            return fail("clip source window out of bounds");
        if (c.sourceDur_ms() < kMinClipMs) return fail("clip below minimum length");
        if (!(c.speed >= kMinSpeed && c.speed <= kMaxSpeed)) return fail("speed out of range");
        if (!(c.volume >= 0.f && c.volume <= kMaxVolume)) return fail("volume out of range");
        if (c.timelineDur_ms() <= 0) return fail("clip has non-positive timeline duration");
        if (c.timelineStart_ms != expect_start) return fail("track is not ripple-packed");
        expect_start += c.timelineDur_ms();
    }
    return true;
}

void ripple(TimelineProject& p) {
    int64_t t = 0;
    for (auto& c : p.clips) {
        c.timelineStart_ms = t;
        t += c.timelineDur_ms();
    }
}

namespace {
TimelineClip* find_clip(TimelineProject& p, const std::string& id) {
    for (auto& c : p.clips) {
        if (c.id == id) return &c;
    }
    return nullptr;
}
bool id_taken(const TimelineProject& p, const std::string& id) {
    for (const auto& c : p.clips) {
        if (c.id == id) return true;
    }
    return false;
}
}  // namespace

bool split(TimelineProject& p, const std::string& clip_id, int64_t t_ms,
           const std::string& new_clip_id) {
    if (new_clip_id.empty() || id_taken(p, new_clip_id)) return false;
    TimelineClip* c = find_clip(p, clip_id);
    if (!c) return false;
    if (t_ms <= c->sourceStart_ms + kSplitMarginMs) return false;
    if (t_ms >= c->sourceEnd_ms - kSplitMarginMs) return false;
    // Both halves must independently satisfy the minimum length — checked
    // BEFORE mutating, so a rejected split leaves the project bit-identical.
    if (t_ms - c->sourceStart_ms < kMinClipMs) return false;
    if (c->sourceEnd_ms - t_ms < kMinClipMs) return false;
    TimelineClip right = *c;
    right.id = new_clip_id;
    right.sourceStart_ms = t_ms;
    c->sourceEnd_ms = t_ms;
    for (auto it = p.clips.begin(); it != p.clips.end(); ++it) {
        if (it->id == clip_id) {
            p.clips.insert(it + 1, right);
            break;
        }
    }
    ripple(p);
    return validate(p, nullptr);
}

bool trim(TimelineProject& p, const std::string& clip_id, int edge, int64_t delta_ms) {
    TimelineClip* c = find_clip(p, clip_id);
    if (!c) return false;
    if (edge != -1 && edge != 1) return false;
    const TimelineSource* src = p.find_source(c->source_id);
    if (!src) return false;
    if (edge < 0) {
        int64_t s = c->sourceStart_ms + delta_ms;
        if (s < 0) s = 0;
        if (s > c->sourceEnd_ms - kMinClipMs) s = c->sourceEnd_ms - kMinClipMs;
        c->sourceStart_ms = s;
    } else {
        int64_t e = c->sourceEnd_ms + delta_ms;
        if (e > src->duration_ms) e = src->duration_ms;
        if (e < c->sourceStart_ms + kMinClipMs) e = c->sourceStart_ms + kMinClipMs;
        c->sourceEnd_ms = e;
    }
    ripple(p);
    return validate(p, nullptr);
}

bool remove(TimelineProject& p, const std::string& clip_id) {
    for (auto it = p.clips.begin(); it != p.clips.end(); ++it) {
        if (it->id == clip_id) {
            p.clips.erase(it);
            ripple(p);
            return validate(p, nullptr);
        }
    }
    return false;
}

bool move(TimelineProject& p, const std::string& clip_id, int64_t to_index) {
    size_t from = p.clips.size();
    for (size_t i = 0; i < p.clips.size(); ++i) {
        if (p.clips[i].id == clip_id) {
            from = i;
            break;
        }
    }
    if (from >= p.clips.size()) return false;
    TimelineClip c = p.clips[from];
    p.clips.erase(p.clips.begin() + static_cast<ptrdiff_t>(from));
    // Signed index: negatives clamp to front (no size_t wraparound).
    size_t to = to_index < 0 ? 0 : static_cast<size_t>(to_index);
    if (to > p.clips.size()) to = p.clips.size();
    p.clips.insert(p.clips.begin() + static_cast<ptrdiff_t>(to), c);
    ripple(p);
    return validate(p, nullptr);
}

}  // namespace cliplite::timeline
