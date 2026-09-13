#include "test_main.h"

#include "cliplite/replay/ring_buffer.h"

using namespace cliplite::replay;

TEST(ring_buffer_retains_within_limit) {
    RingBuffer rb(60000);  // 60 s
    rb.add_segment(0, 10000, "seg1");
    rb.add_segment(10000, 20000, "seg2");
    rb.add_segment(20000, 30000, "seg3");
    CHECK_EQ(rb.size(), 3u);
    CHECK_EQ(rb.total_duration_ms(), 30000);
    CHECK_EQ(rb.span_start_ms(), 0);
    CHECK_EQ(rb.span_end_ms(), 30000);
}

TEST(ring_buffer_evicts_oldest) {
    RingBuffer rb(30000);
    for (int i = 0; i < 10; ++i) rb.add_segment(i * 10000, (i + 1) * 10000, "seg");
    CHECK_EQ(rb.total_duration_ms(), 30000);
    CHECK_EQ(rb.size(), 3u);
    CHECK_EQ(rb.span_start_ms(), 70000);
    CHECK_EQ(rb.span_end_ms(), 100000);
}

TEST(ring_buffer_collect_full_window) {
    RingBuffer rb(60000);
    for (int i = 0; i < 6; ++i) rb.add_segment(i * 10000, (i + 1) * 10000, "seg");
    auto segs = rb.collect(0, 60000);
    int64_t total = 0;
    for (const auto& s : segs) total += s.duration_ms();
    CHECK_EQ(total, 60000);
    CHECK_EQ(segs.size(), 6u);
}

TEST(ring_buffer_collect_partial_window) {
    RingBuffer rb(60000);
    rb.add_segment(0, 10000, "seg");
    // Save 60 s but only 10 s are buffered.
    auto segs = rb.collect(-50000, 10000);
    CHECK_EQ(segs.size(), 1u);
    CHECK_EQ(segs[0].duration_ms(), 10000);
}

TEST(ring_buffer_collect_empty_range) {
    RingBuffer rb(60000);
    rb.add_segment(0, 10000, "seg");
    auto segs = rb.collect(5000, 5000);
    CHECK(segs.empty());
}

TEST(ring_buffer_ignores_invalid_segment) {
    RingBuffer rb(60000);
    rb.add_segment(5000, 5000, "bad");
    rb.add_segment(5000, 4000, "bad2");
    CHECK(rb.empty());
    CHECK_EQ(rb.total_duration_ms(), 0);
}

TEST(ring_buffer_partial_trim_keeps_tail) {
    RingBuffer rb(25000);
    rb.add_segment(0, 10000, "a");
    rb.add_segment(10000, 20000, "b");
    rb.add_segment(20000, 30000, "c");
    // Excess of 5 s trims the head segment from the front.
    CHECK_EQ(rb.total_duration_ms(), 25000);
    CHECK_EQ(rb.span_start_ms(), 5000);
    CHECK_EQ(rb.span_end_ms(), 30000);
    CHECK_EQ(rb.size(), 3u);
}

TEST(ring_buffer_prune_preserves_file_start) {
    // Regression test for long clips: prune() slides the head segment's
    // window start forward, but the file still begins at orig_start_ms.
    // save_clip must trim the head file by (start - orig_start).
    RingBuffer rb(20000);
    rb.add_segment(0, 10000, "a");
    rb.add_segment(10000, 20000, "b");
    rb.add_segment(20000, 30000, "c");  // total 30000 > 20000: excess 10000
    CHECK_EQ(rb.size(), 2u);
    const auto segs = rb.segments();
    CHECK_EQ(segs[0].path, std::string("b"));
    CHECK_EQ(segs[0].start_ms, 10000);
    CHECK_EQ(segs[0].orig_start_ms, 10000);  // unpruned file: identical
    // Partial trim case: front keeps its bytes, window slides.
    RingBuffer rb2(25000);
    rb2.add_segment(0, 10000, "a");
    rb2.add_segment(10000, 20000, "b");
    rb2.add_segment(20000, 30000, "c");  // excess 5000: a becomes [5000,10000)
    const auto s2 = rb2.segments();
    CHECK_EQ(s2[0].path, std::string("a"));
    CHECK_EQ(s2[0].start_ms, 5000);
    CHECK_EQ(s2[0].orig_start_ms, 0);  // file still starts at 0
    // The save window [5000,25000) needs a 5000ms head trim of file "a".
    const auto got = rb2.collect(5000, 25000);
    CHECK(!got.empty());
    CHECK_EQ(got.front().start_ms - s2[0].orig_start_ms, 5000);
}
