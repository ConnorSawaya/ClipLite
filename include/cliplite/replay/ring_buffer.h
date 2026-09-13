#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace cliplite::replay {

// Metadata for one short encoded segment on disk. Times are milliseconds on a
// monotonic capture timeline.
struct Segment {
    int64_t id = 0;
    int64_t start_ms = 0;  // inclusive (moves forward when prune() trims the head)
    int64_t end_ms = 0;    // exclusive
    int64_t orig_start_ms = 0;  // the file's TRUE start; never moves after add.
                                // prune() slides start_ms but the bytes before
                                // it stay on disk, so save_clip must trim the
                                // head file by (start_ms - orig_start_ms).
    std::string path;

    int64_t duration_ms() const { return end_ms - start_ms; }
};

// Circular replay buffer of encoded segments. Tracks total buffered duration and
// evicts the oldest data when max_duration_ms is exceeded. Pure logic: no file I/O,
// so it can be unit-tested independently of the capture/encoder pipeline.
class RingBuffer {
public:
    explicit RingBuffer(int64_t max_duration_ms);

    void add_segment(const Segment& seg);
    void add_segment(int64_t start_ms, int64_t end_ms, const std::string& path);

    // Returns segments overlapping [start_ms, end_ms), clipped to the requested
    // window. Used to assemble the previous N seconds on clip save.
    std::vector<Segment> collect(int64_t start_ms, int64_t end_ms) const;

    int64_t max_duration_ms() const { return max_duration_ms_; }
    int64_t total_duration_ms() const { return total_duration_ms_; }
    int64_t span_start_ms() const { return span_start_ms_; }
    int64_t span_end_ms() const { return span_end_ms_; }
    std::size_t size() const { return segments_.size(); }
    bool empty() const { return segments_.empty(); }
    // Snapshot of stored segments with ORIGINAL (unclipped) boundaries, used by
    // save_clip to compute head/tail trims for exact-length remuxing.
    std::vector<Segment> segments() const {
        return std::vector<Segment>(segments_.begin(), segments_.end());
    }

private:
    void prune();

    int64_t max_duration_ms_;
    int64_t total_duration_ms_ = 0;
    int64_t span_start_ms_ = 0;
    int64_t span_end_ms_ = 0;
    int64_t next_id_ = 1;
    std::deque<Segment> segments_;
};

}  // namespace cliplite::replay
