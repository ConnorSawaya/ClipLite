#include "cliplite/replay/ring_buffer.h"

#include <algorithm>

namespace cliplite::replay {

RingBuffer::RingBuffer(int64_t max_duration_ms)
    : max_duration_ms_(max_duration_ms < 0 ? 0 : max_duration_ms) {}

void RingBuffer::add_segment(const Segment& seg) {
    add_segment(seg.start_ms, seg.end_ms, seg.path);
}

void RingBuffer::add_segment(int64_t start_ms, int64_t end_ms, const std::string& path) {
    if (end_ms <= start_ms) return;
    Segment seg;
    seg.id = next_id_++;
    seg.start_ms = start_ms;
    seg.end_ms = end_ms;
    seg.orig_start_ms = start_ms;
    seg.path = path;

    if (segments_.empty()) span_start_ms_ = seg.start_ms;
    segments_.push_back(seg);
    total_duration_ms_ += seg.duration_ms();
    span_end_ms_ = seg.end_ms;
    prune();
}

void RingBuffer::prune() {
    while (!segments_.empty() && total_duration_ms_ > max_duration_ms_) {
        const Segment front = segments_.front();
        const int64_t excess = total_duration_ms_ - max_duration_ms_;
        if (front.duration_ms() <= excess) {
            total_duration_ms_ -= front.duration_ms();
            segments_.pop_front();
        } else {
            segments_.front().start_ms = front.end_ms - (front.duration_ms() - excess);
            total_duration_ms_ = max_duration_ms_;
        }
    }
    if (segments_.empty()) {
        span_start_ms_ = 0;
        span_end_ms_ = 0;
    } else {
        span_start_ms_ = segments_.front().start_ms;
        span_end_ms_ = segments_.back().end_ms;
    }
}

std::vector<Segment> RingBuffer::collect(int64_t start_ms, int64_t end_ms) const {
    std::vector<Segment> out;
    if (start_ms >= end_ms) return out;
    for (const auto& seg : segments_) {
        const int64_t s = std::max(seg.start_ms, start_ms);
        const int64_t e = std::min(seg.end_ms, end_ms);
        if (e > s) {
            Segment clipped = seg;
            clipped.start_ms = s;
            clipped.end_ms = e;
            out.push_back(clipped);
        }
    }
    return out;
}

}  // namespace cliplite::replay
