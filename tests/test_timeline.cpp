#include "test_main.h"

#include "cliplite/library/timeline.h"

using namespace cliplite::timeline;

namespace {
TimelineProject one_clip_project() {
    TimelineProject p;
    p.sources.push_back({"s1", 10000});
    p.clips.push_back({"c1", "s1", 0, 10000, 0, 1.0, 1.0f, false});
    ripple(p);
    return p;
}
}  // namespace

TEST(timeline_validate_and_ripple) {
    auto p = one_clip_project();
    CHECK(validate(p, nullptr));
    CHECK_EQ(p.duration_ms(), 10000);
    CHECK_EQ(p.clips[0].timelineStart_ms, 0);
}

TEST(timeline_split_preserves_total) {
    auto p = one_clip_project();
    CHECK(split(p, "c1", 4000, "c2"));
    CHECK_EQ(p.clips.size(), 2u);
    CHECK_EQ(p.clips[0].sourceStart_ms, 0);
    CHECK_EQ(p.clips[0].sourceEnd_ms, 4000);
    CHECK_EQ(p.clips[1].sourceStart_ms, 4000);
    CHECK_EQ(p.clips[1].sourceEnd_ms, 10000);
    CHECK_EQ(p.duration_ms(), 10000);
    CHECK_EQ(p.clips[1].timelineStart_ms, 4000);
    CHECK(validate(p, nullptr));
    // Margins: too close to either edge is a no-op.
    CHECK(!split(p, "c1", 10, "cx"));
    CHECK(!split(p, "c1", 3990, "cx"));
    CHECK(!split(p, "nope", 2000, "cx"));
    CHECK(!split(p, "c1", 2000, "c2"));  // id taken
}

TEST(timeline_trim_clamps) {
    auto p = one_clip_project();
    CHECK(split(p, "c1", 5000, "c2"));
    CHECK(trim(p, "c1", 1, -1000));  // right edge in to 4000
    CHECK_EQ(p.clips[0].sourceEnd_ms, 4000);
    CHECK_EQ(p.duration_ms(), 9000);
    CHECK_EQ(p.clips[1].timelineStart_ms, 4000);
    CHECK(trim(p, "c2", -1, 6000));  // overshoots: clamps at min length
    CHECK_EQ(p.clips[1].sourceStart_ms, 9900);
    CHECK_EQ(p.clips[1].sourceDur_ms(), kMinClipMs);
    CHECK(trim(p, "c2", -1, -20000));  // clamp to source start
    CHECK_EQ(p.clips[1].sourceStart_ms, 0);
    CHECK(!trim(p, "nope", 1, 100));
    CHECK(!trim(p, "c1", 0, 100));
    CHECK(validate(p, nullptr));
}

TEST(timeline_delete_ripples) {
    auto p = one_clip_project();
    CHECK(split(p, "c1", 3000, "c2"));
    CHECK(split(p, "c2", 7000, "c3"));
    CHECK_EQ(p.duration_ms(), 10000);
    CHECK(remove(p, "c2"));
    CHECK_EQ(p.clips.size(), 2u);
    CHECK_EQ(p.duration_ms(), 6000);
    CHECK_EQ(p.clips[1].timelineStart_ms, 3000);
    CHECK_EQ(p.clips[1].sourceStart_ms, 7000);
    CHECK(!remove(p, "nope"));
    CHECK(validate(p, nullptr));
}

TEST(timeline_failed_split_leaves_identical) {
    auto p = one_clip_project();
    CHECK(!split(p, "c1", 30, "c2"));  // 30ms half < 100ms minimum
    CHECK_EQ(p.clips.size(), 1u);
    CHECK_EQ(p.clips[0].sourceStart_ms, 0);
    CHECK_EQ(p.clips[0].sourceEnd_ms, 10000);
    CHECK(validate(p, nullptr));
    // Short source: any split position fails, nothing mutates.
    TimelineProject q;
    q.sources.push_back({"s1", 150});
    q.clips.push_back({"c1", "s1", 0, 150, 0, 1.0, 1.0f, false});
    CHECK(!split(q, "c1", 75, "c2"));
    CHECK_EQ(q.clips.size(), 1u);
    CHECK_EQ(q.clips[0].sourceEnd_ms, 150);
}

TEST(timeline_rejects_duplicate_ids) {
    auto p = one_clip_project();
    p.clips.push_back({"c1", "s1", 0, 1000, 0, 1.0, 1.0f, false});
    std::string err;
    CHECK(!validate(p, &err));
    CHECK(!err.empty());
}

TEST(timeline_move_reorders) {
    auto p = one_clip_project();
    CHECK(split(p, "c1", 3000, "c2"));
    CHECK(split(p, "c2", 7000, "c3"));
    CHECK(move(p, "c3", 0));
    CHECK_EQ(p.clips[0].id, std::string("c3"));
    CHECK_EQ(p.clips[0].timelineStart_ms, 0);
    CHECK_EQ(p.duration_ms(), 10000);
    CHECK(!move(p, "nope", 0));
    CHECK(validate(p, nullptr));
}

TEST(timeline_rejects_bad_state) {
    TimelineProject p;
    std::string err;
    CHECK(!validate(p, &err));  // no sources
    p.sources.push_back({"s1", 1000});
    p.clips.push_back({"c1", "missing", 0, 500, 0, 1.0, 1.0f, false});
    CHECK(!validate(p, &err));
    p.clips[0].source_id = "s1";
    p.clips[0].sourceEnd_ms = 5000;  // beyond source
    CHECK(!validate(p, &err));
    p.clips[0].sourceEnd_ms = 50;  // below min length
    CHECK(!validate(p, &err));
    p.clips[0].sourceEnd_ms = 500;
    p.clips[0].speed = 0.0;
    CHECK(!validate(p, &err));
    p.clips[0].speed = 1.0;
    p.clips[0].timelineStart_ms = 7;  // not ripple-packed
    CHECK(!validate(p, &err));
}
