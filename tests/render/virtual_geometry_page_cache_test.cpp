// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// CPU proofs for the M18.5 page-cache policy, on a FAKE frame clock: every begin_frame() states
// the retirement watermark explicitly, so "the GPU is three frames behind" is a number in the test
// rather than a race. The proofs:
//
//   (a) a page is resident only once its upload's frame has retired — never at issue time;
//   (b) a slot is reused only once every frame that read its old page has retired;
//   (c) the permanent coarse cut is never a victim, and a pool too small for it is refused;
//   (d) a frame never uploads more than its budget, and what does not fit is counted, in priority
//       order; LRU picks the victim; dependencies pin and are uploaded first;
//   (e) a randomized long run with a lagging, jittering GPU, checked against a fake GPU that
//       remembers which slots each in-flight frame read — the proofs above as invariants, not
//       as hand-picked sequences.

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "rime/render/virtual_geometry_page_cache.hpp"
#include "vg_streaming_fixture.hpp"

namespace {

using namespace rime;
using namespace rime::render;
namespace fx = rime::test::vg_streaming;

constexpr assets::AssetId kA{101};
constexpr assets::AssetId kB{102};
constexpr std::uint32_t kSlotBytes = 192;  // 176-byte pages, rounded to whole 32-byte vertices
constexpr std::uint32_t kBudget = 2 * 192; // two pages per frame (2 * 176 fit, a third does not)

VirtualGeometryPageCacheConfig
config(std::uint32_t slots, std::uint32_t segments = 3, std::uint32_t budget = kBudget) {
    return {slots, kSlotBytes, budget, segments};
}

struct Registered {
    assets::VirtualGeometryAsset asset = fx::tree();
    std::vector<VirtualGeometryPageUpload> permanent;
};

// Run one frame with the given requests (page, priority) and reads, returning the plan.
std::vector<VirtualGeometryPageUpload>
frame(VirtualGeometryPageCache& cache,
      VirtualGeometryFrame current,
      VirtualGeometryFrame retired,
      std::vector<std::pair<std::uint32_t, std::uint32_t>> requests,
      std::vector<std::uint32_t> reads = {},
      assets::AssetId id = kA) {
    cache.begin_frame(current, retired);
    for (const std::uint32_t page : reads) {
        cache.mark_used(id, page);
    }
    for (const auto& [page, priority] : requests) {
        cache.request(id, page, priority);
    }
    const auto plan = cache.plan_uploads();
    return {plan.begin(), plan.end()};
}

} // namespace

TEST_CASE("vg page cache: registration pins the coarse cut and refuses what cannot stream") {
    Registered r;
    SUBCASE("the permanent page takes the first slot and is resident immediately") {
        VirtualGeometryPageCache cache(config(8));
        REQUIRE(cache.is_valid());
        REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
        REQUIRE(r.permanent.size() == 1);
        CHECK(r.permanent[0].page == 0);
        CHECK(r.permanent[0].slot == 0);
        CHECK(r.permanent[0].bytes == fx::kPageBytes);
        CHECK(cache.is_resident(kA, 0));
        CHECK(cache.resident_slot(kA, 0) == std::optional<std::uint32_t>{0});
        CHECK_FALSE(cache.is_resident(kA, 1));
        CHECK(cache.stats().permanent_pages == 1);
        CHECK(cache.free_slots() == 7);
        // Same object under the same id is idempotent; a different object is a conflict.
        CHECK(cache.register_asset(kA, r.asset, r.permanent));
        const assets::VirtualGeometryAsset other = fx::tree();
        CHECK_FALSE(cache.register_asset(kA, other, r.permanent));
    }
    SUBCASE("a pool with no slot left for a second coarse cut refuses the asset") {
        VirtualGeometryPageCache cache(config(1));
        REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
        CHECK_FALSE(cache.register_asset(kB, r.asset, r.permanent));
    }
    SUBCASE("slots too small for a page, or not a whole number of vertices, are refused") {
        VirtualGeometryPageCache small({8, 160, 320, 3});
        CHECK_FALSE(small.register_asset(kA, r.asset, r.permanent)); // 176-byte pages
        VirtualGeometryPageCache ragged({8, 200, 400, 3});
        CHECK_FALSE(ragged.register_asset(kA, r.asset, r.permanent)); // 200 % 32 != 0
    }
    SUBCASE("a budget below one slot could never upload a full page: invalid config") {
        CHECK_FALSE(VirtualGeometryPageCacheConfig{8, 192, 191, 3}.is_valid());
        CHECK_FALSE(VirtualGeometryPageCacheConfig{0, 192, 384, 3}.is_valid());
        CHECK_FALSE(VirtualGeometryPageCacheConfig{8, 192, 384, 0}.is_valid());
    }
}

TEST_CASE("vg page cache: a page is resident only once the frame that uploaded it has retired") {
    Registered r;
    VirtualGeometryPageCache cache(config(8));
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));

    const auto plan = frame(cache, 1, 0, {{1, 1}});
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].page == 1);
    CHECK(plan[0].frame == 1);
    CHECK(plan[0].slot == 1);
    // Issued is not holding: the copy is merely recorded.
    CHECK(cache.is_uploading(kA, 1));
    CHECK_FALSE(cache.is_resident(kA, 1));
    CHECK_FALSE(cache.resident_slot(kA, 1).has_value());
    CHECK(cache.page_residency_bytes(kA)[1] == 0);
    CHECK_FALSE(cache.residency().is_resident(kA, 1));

    // Two more frames recorded, the GPU still has not finished frame 1.
    frame(cache, 2, 0, {{1, 1}});
    CHECK_FALSE(cache.is_resident(kA, 1));
    CHECK(cache.stats().requests_in_flight == 1); // the repeat request created no second upload
    frame(cache, 3, 0, {});
    CHECK_FALSE(cache.is_resident(kA, 1));
    CHECK(cache.stats().uploads_retired == 0);

    // Frame 1's fence has signalled.
    frame(cache, 4, 1, {});
    CHECK(cache.is_resident(kA, 1));
    CHECK(cache.resident_slot(kA, 1) == std::optional<std::uint32_t>{1});
    CHECK(cache.page_residency_bytes(kA)[1] == 1);
    CHECK(cache.residency().is_resident(kA, 1));
    CHECK(cache.stats().uploads_retired == 1);
    CHECK(cache.stats().uploads_issued == 1);
}

TEST_CASE("vg page cache: a slot is reused only after every frame that read its page retired") {
    Registered r;
    VirtualGeometryPageCache cache(config(2)); // the coarse slot + ONE streaming slot
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
    REQUIRE(frame(cache, 1, 0, {{1, 1}}).size() == 1);
    frame(cache, 2, 1, {}, {1}); // page 1 resident now, and frame 2 draws it
    REQUIRE(cache.is_resident(kA, 1));
    frame(cache, 3, 1, {}, {1}); // frame 3 draws it too
    const std::uint32_t generation = cache.slot_generation(1);

    // Frame 4 wants page 2. The only candidate slot was read by frame 3, which has not retired.
    CHECK(frame(cache, 4, 2, {{2, 1}}).empty());
    CHECK(cache.stats().deferred_no_slot == 1);
    CHECK(cache.stats().evictions == 0);
    CHECK(cache.is_resident(kA, 1));

    // Frame 3 retires: page 1 is evicted and page 2 takes its slot with a new generation.
    const auto plan = frame(cache, 5, 3, {{2, 1}});
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].page == 2);
    CHECK(plan[0].slot == 1);
    CHECK(plan[0].generation == generation + 1);
    CHECK(cache.stats().evictions == 1);
    CHECK_FALSE(cache.is_resident(kA, 1));
    CHECK_FALSE(cache.residency().is_resident(kA, 1));
    CHECK_FALSE(cache.is_resident(kA, 2)); // and page 2 waits for frame 5, as ever
}

TEST_CASE("vg page cache: the permanent coarse cut is never evicted, however hard it is pushed") {
    Registered r;
    SUBCASE("a pool of only the coarse slot defers every request and evicts nothing") {
        VirtualGeometryPageCache cache(config(1));
        REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
        for (VirtualGeometryFrame f = 1; f <= 10; ++f) {
            CHECK(frame(cache, f, f - 1, {{1, 1}, {2, 1}}).empty());
        }
        CHECK(cache.stats().evictions == 0);
        CHECK(cache.stats().deferred_no_slot == 20);
        CHECK(cache.is_resident(kA, 0));
        CHECK(cache.resident_slot(kA, 0) == std::optional<std::uint32_t>{0});
    }
    SUBCASE("churn through one streaming slot: every other page cycles, the coarse page stays") {
        VirtualGeometryPageCache cache(config(2));
        REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
        for (VirtualGeometryFrame f = 1; f <= 60; ++f) {
            const std::uint32_t wanted = 1 + static_cast<std::uint32_t>(f % 6);
            frame(cache, f, f - 1, {{wanted, 0}}, {0});
            CHECK(cache.is_resident(kA, 0));
            CHECK(cache.resident_slot(kA, 0) == std::optional<std::uint32_t>{0});
            CHECK(cache.slot_generation(0) == 1); // never rewritten
        }
        CHECK(cache.stats().evictions > 10);
    }
}

TEST_CASE("vg page cache: the per-frame budget is a hard ceiling, served in priority order") {
    Registered r;
    VirtualGeometryPageCache cache(config(8));
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));

    // Six pages wanted, two fit. Priorities pick pages 6 and 2 even though they were asked last.
    const auto plan = frame(cache, 1, 0, {{1, 5}, {3, 5}, {4, 5}, {5, 5}, {2, 1}, {6, 0}});
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].page == 6);
    CHECK(plan[1].page == 2);
    CHECK(cache.stats().frame_upload_bytes == 2 * fx::kPageBytes);
    CHECK(cache.stats().frame_upload_bytes <= kBudget);
    CHECK(cache.stats().deferred_budget == 4);
    // Staging offsets sit back to back inside frame 1's segment (segment 1 of 3).
    CHECK(plan[0].staging_offset == kBudget);
    CHECK(plan[1].staging_offset == kBudget + fx::kPageBytes);

    // Draining the rest takes two more frames, never more than the budget in any one.
    std::uint32_t total = 2;
    for (VirtualGeometryFrame f = 2; f <= 4; ++f) {
        const auto more = frame(cache, f, f - 1, {{1, 5}, {3, 5}, {4, 5}, {5, 5}});
        total += static_cast<std::uint32_t>(more.size());
        CHECK(cache.stats().frame_upload_bytes <= kBudget);
    }
    CHECK(total == 6);
    CHECK(cache.stats().max_frame_upload_bytes <= kBudget);
}

TEST_CASE("vg page cache: a staging segment still in flight defers the whole frame") {
    Registered r;
    VirtualGeometryPageCache cache(config(8, /*segments=*/1));
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
    REQUIRE(frame(cache, 1, 0, {{1, 0}}).size() == 1);
    // Frame 1 may still be copying out of the one segment.
    CHECK(frame(cache, 2, 0, {{2, 0}, {3, 0}}).empty());
    CHECK(cache.stats().deferred_staging_busy == 2);
    CHECK(frame(cache, 3, 1, {{2, 0}, {3, 0}}).size() == 2);
}

TEST_CASE("vg page cache: LRU picks the victim, and a page wanted this frame is never one") {
    Registered r;
    VirtualGeometryPageCache cache(config(3)); // coarse + two streaming slots
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
    REQUIRE(frame(cache, 1, 0, {{1, 0}, {2, 0}}).size() == 2);
    frame(cache, 2, 1, {}, {2});
    frame(cache, 3, 2, {}, {1}); // page 1 read more recently than page 2
    const auto plan = frame(cache, 5, 4, {{3, 0}});
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].slot == 2); // page 2's slot
    CHECK(cache.is_resident(kA, 1));
    CHECK_FALSE(cache.is_resident(kA, 2));

    // Page 1 is now the older resident page, but this frame wants it: page 3 (retired) goes.
    const auto next = frame(cache, 7, 6, {{1, 0}, {4, 0}});
    REQUIRE(next.size() == 1);
    CHECK(next[0].page == 4);
    CHECK(cache.is_resident(kA, 1));
    CHECK_FALSE(cache.is_resident(kA, 3));
    CHECK(cache.stats().requests_satisfied >= 1);
}

TEST_CASE("vg page cache: dependencies travel first and pin the pages they need") {
    Registered r;
    // Page 3 depends on page 1 (as a shared-vertex page would).
    r.asset.page_dependencies = {1};
    r.asset.pages[3].first_dependency = 0;
    r.asset.pages[3].dependency_count = 1;
    REQUIRE(assets::validate_virtual_geometry(r.asset) == assets::VirtualGeometryError::None);
    VirtualGeometryPageCache cache(config(3));
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));

    CHECK(frame(cache, 1, 0, {{3, 0}}).empty());
    CHECK(cache.stats().deferred_dependency == 1);

    // Requested together (dependency at a better priority): both go in one frame, and both are
    // promoted by the same retirement even though the residency contract wants page 1 first.
    REQUIRE(frame(cache, 2, 1, {{3, 1}, {1, 0}}).size() == 2);
    frame(cache, 3, 2, {});
    CHECK(cache.is_resident(kA, 1));
    CHECK(cache.is_resident(kA, 3));

    // The pool is full; page 1 is old and unread, but page 3 still depends on it.
    const auto plan = frame(cache, 10, 9, {{2, 0}});
    REQUIRE(plan.size() == 1);
    CHECK(cache.is_resident(kA, 1));
    CHECK_FALSE(cache.is_resident(kA, 3)); // the dependent went instead
}

TEST_CASE("vg page cache: a clock that claims too much is ignored, and a failed frame is undone") {
    Registered r;
    VirtualGeometryPageCache cache(config(8));
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
    REQUIRE(frame(cache, 1, 0, {{1, 0}}).size() == 1);

    SUBCASE("retired_through at or past the current frame cannot be true") {
        cache.begin_frame(2, 5);
        CHECK(cache.stats().clock_rejected == 1);
        CHECK(cache.retired_through() == 0);
        CHECK_FALSE(cache.is_resident(kA, 1));
        cache.begin_frame(2, 1); // a repeated frame number advances anyway, and is counted
        CHECK(cache.stats().clock_rejected == 2);
        CHECK(cache.current_frame() == 3);
        CHECK(cache.is_resident(kA, 1));
    }
    SUBCASE("abandon_frame returns the frame's uploads to absent and their slots to the pool") {
        const std::uint32_t free_before = cache.free_slots();
        cache.abandon_frame(1);
        CHECK(cache.stats().uploads_abandoned == 1);
        CHECK_FALSE(cache.is_uploading(kA, 1));
        CHECK(cache.free_slots() == free_before + 1);
        frame(cache, 2, 1, {});
        CHECK_FALSE(cache.is_resident(kA, 1));
    }
}

TEST_CASE("vg page cache: a teleport falls back to the coarse cut, then converges") {
    Registered r;
    // Room for both coarse cuts and ONE instance's six streaming pages: B can only arrive by
    // evicting A, and only once A's last reads have retired.
    VirtualGeometryPageCache cache(config(8));
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
    REQUIRE(cache.register_asset(kB, r.asset, r.permanent));
    const std::vector<std::uint32_t> leaves{3, 4, 5, 6};

    VirtualGeometryFrame f = 1;
    constexpr VirtualGeometryFrame kLag = 2; // the GPU finishes a frame two frames later
    const auto tick = [&](assets::AssetId id) {
        cache.begin_frame(f, f > kLag ? f - kLag : 0);
        const VirtualGeometrySelection s =
            stream_virtual_geometry(cache, id, r.asset, fx::kNear, 1.0f);
        (void)cache.plan_uploads();
        ++f;
        return s;
    };
    for (int i = 0; i < 20 && tick(kA).groups != leaves; ++i) {
    }
    REQUIRE(tick(kA).groups == leaves);

    const std::uint64_t fallbacks = cache.stats().fallbacks;
    const std::uint64_t evictions = cache.stats().evictions;
    const VirtualGeometrySelection first = tick(kB);
    CHECK(first.groups == std::vector<std::uint32_t>{0}); // coarse, not nothing
    CHECK(cache.stats().fallbacks == fallbacks + 1);
    std::uint32_t frames = 1;
    VirtualGeometrySelection s = first;
    while (s.groups != leaves && frames < 30) {
        s = tick(kB);
        CHECK_FALSE(s.groups.empty());
        CHECK(cache.stats().frame_upload_bytes <= kBudget);
        ++frames;
    }
    CHECK(s.groups == leaves);
    CHECK(cache.stats().evictions == evictions + 6);
    CHECK(cache.stats().deferred_budget > 0);
    MESSAGE("teleport converged in " << frames << " frames");
}

TEST_CASE("vg page cache: a long randomized run against a fake GPU breaks no rule") {
    Registered r;
    VirtualGeometryPageCache cache(config(6)); // two coarse slots + four streaming
    REQUIRE(cache.register_asset(kA, r.asset, r.permanent));
    REQUIRE(cache.register_asset(kB, r.asset, r.permanent));
    const std::map<std::uint64_t, std::uint32_t> coarse_slot = {
        {kA.value, *cache.resident_slot(kA, 0)}, {kB.value, *cache.resident_slot(kB, 0)}};

    // The fake GPU: which slots each frame's draws read, and which frame uploaded each page.
    std::map<VirtualGeometryFrame, std::set<std::uint32_t>> reads;
    std::map<std::pair<std::uint64_t, std::uint32_t>, VirtualGeometryFrame> uploaded_in;
    std::vector<VirtualGeometryFrame> segment_user(3, 0);

    std::uint32_t rng = 12345u; // LCG: the run is random-looking and exactly reproducible
    const auto next = [&rng](std::uint32_t n) {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) % n;
    };
    VirtualGeometryFrame retired = 0;
    for (VirtualGeometryFrame f = 1; f <= 600; ++f) {
        // A GPU that lags 0..4 frames and catches up in bursts, never retiring backwards.
        const VirtualGeometryFrame lag = next(5);
        retired = std::max(retired, f > lag + 1 ? f - lag - 1 : VirtualGeometryFrame{0});
        cache.begin_frame(f, retired);

        // Rule 1, as an invariant: nothing is resident whose upload has not retired.
        for (const auto& [key, frame_uploaded] : uploaded_in) {
            if (cache.is_resident(assets::AssetId{key.first}, key.second)) {
                CHECK(frame_uploaded <= retired);
            }
        }

        for (const assets::AssetId id : {kA, kB}) {
            const float ppm = next(3) == 0 ? fx::kFar : fx::kNear;
            const VirtualGeometrySelection s =
                stream_virtual_geometry(cache, id, r.asset, ppm, 1.0f);
            REQUIRE_FALSE(s.groups.empty()); // hole-free: a cut always exists
            for (const std::uint32_t g : s.groups) {
                const auto slot = cache.resident_slot(id, r.asset.clusters[g].page);
                REQUIRE(slot.has_value()); // the drawn cut is entirely confirmed-resident
                reads[f].insert(*slot);
            }
        }

        const auto plan = cache.plan_uploads();
        std::uint64_t bytes = 0;
        const std::uint32_t segment = static_cast<std::uint32_t>(f % 3);
        if (!plan.empty()) {
            CHECK(segment_user[segment] <= retired); // staging reuse waits for retirement too
            segment_user[segment] = f;
        }
        for (const VirtualGeometryPageUpload& u : plan) {
            // Rule 2, as an invariant: no frame that has not retired read this slot.
            for (VirtualGeometryFrame g = retired + 1; g <= f; ++g) {
                CHECK(reads[g].count(u.slot) == 0);
            }
            CHECK(u.slot != coarse_slot.at(u.asset.value)); // never the coarse cut's slot
            CHECK(u.staging_offset >= std::uint64_t{segment} * kBudget);
            CHECK(u.staging_offset + u.bytes <= std::uint64_t{segment + 1} * kBudget);
            bytes += u.bytes;
            uploaded_in[{u.asset.value, u.page}] = f;
        }
        CHECK(bytes <= kBudget);
        CHECK(cache.is_resident(kA, 0));
        CHECK(cache.is_resident(kB, 0));
        reads.erase(reads.begin(), reads.lower_bound(retired + 1));
    }
    const VirtualGeometryPageCacheStats& st = cache.stats();
    CHECK(st.max_frame_upload_bytes <= kBudget);
    CHECK(st.use_of_nonresident == 0);
    CHECK(st.clock_rejected == 0);
    // The run must actually have exercised every path it claims to prove.
    CHECK(st.evictions > 20);
    CHECK(st.uploads_retired > 20);
    CHECK(st.fallbacks > 20);
    CHECK(st.deferred_budget > 0);
    CHECK(st.deferred_no_slot > 0);
    MESSAGE("requests " << st.requests << ", uploads " << st.uploads_issued << ", evictions "
                        << st.evictions << ", deferred budget/slot/staging " << st.deferred_budget
                        << "/" << st.deferred_no_slot << "/" << st.deferred_staging_busy
                        << ", fallbacks " << st.fallbacks);
}
