// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <vector>

#include "../../engine/render/src/shadow_caster_tracker.hpp"

using namespace rime;
using render::detail::ShadowCasterRecord;
using render::detail::ShadowCasterTracker;

namespace {
struct Invalidations {
    std::vector<render::WorldAabb> regions;
    unsigned all = 0;
};

Invalidations update(ShadowCasterTracker& tracker, std::span<const ShadowCasterRecord> records) {
    Invalidations result;
    tracker.update(
        records,
        [&](const render::WorldAabb& bounds) { result.regions.push_back(bounds); },
        [&] { ++result.all; });
    return result;
}

ShadowCasterRecord caster(std::uint64_t entity = 1, float x = 0) {
    ShadowCasterRecord record;
    record.entity = entity;
    record.mesh = 3;
    record.index_count = 36;
    record.model = core::mat4_translation({x, 0, 0});
    record.bounds = {{x, 0, 0}, {x + 1, 1, 1}};
    return record;
}

void same_box(const render::WorldAabb& a, const render::WorldAabb& b) {
    CHECK(a.min.x == b.min.x);
    CHECK(a.min.y == b.min.y);
    CHECK(a.min.z == b.min.z);
    CHECK(a.max.x == b.max.x);
    CHECK(a.max.y == b.max.y);
    CHECK(a.max.z == b.max.z);
}

void unchanged(ShadowCasterTracker& tracker, std::span<const ShadowCasterRecord> records) {
    auto result = update(tracker, records);
    CHECK(result.all == 0);
    CHECK(result.regions.empty());
}
} // namespace

TEST_CASE("shadow caster tracking ignores draw reorder and camera independent unchanged records") {
    ShadowCasterTracker tracker;
    std::array records{caster(1, -4), caster(2, 5), caster(3, 9)};
    const auto first = update(tracker, records);
    CHECK(first.all == 0);
    REQUIRE(first.regions.size() == 3);
    CHECK(tracker.size() == records.size());
    unchanged(tracker, records);
    // A camera can reveal another subset, but the caller supplies the complete caster set. Its
    // order, projection and lighting never become part of depth identity for a stationary lamp.
    for (int frame = 0; frame < 20; ++frame) {
        std::rotate(records.begin(), records.begin() + 1, records.end());
        unchanged(tracker, records);
    }
}

TEST_CASE("shadow caster movement invalidates old and new footprints then settles") {
    ShadowCasterTracker tracker;
    std::array records{caster()};
    update(tracker, records);
    const auto before = records[0].bounds;
    records[0].model = core::mat4_translation({7, 2, -1});
    records[0].bounds = {{7, 2, -1}, {8, 3, 0}};
    const auto moved = update(tracker, records);
    CHECK(moved.all == 0);
    REQUIRE(moved.regions.size() == 2);
    same_box(moved.regions[0], before);
    same_box(moved.regions[1], records[0].bounds);
    unchanged(tracker, records);
    // Symmetric bounds alone do not prove an unchanged pose: rotation can move shadow geometry
    // while leaving a conservative enclosing box identical.
    records[0].model.m[0] = -1;
    CHECK(update(tracker, records).regions.size() == 2);
    unchanged(tracker, records);
}

TEST_CASE("shadow caster removal reappearance and full generational identities invalidate") {
    ShadowCasterTracker tracker;
    std::array records{caster()};
    update(tracker, records);
    auto removed = update(tracker, {});
    CHECK(removed.all == 0);
    REQUIRE(removed.regions.size() == 1);
    same_box(removed.regions[0], records[0].bounds);
    CHECK(tracker.size() == 0);
    unchanged(tracker, {});
    CHECK(update(tracker, records).regions.size() == 1);
    // Same slot, next generation: these are two different casters, even at an identical pose.
    records[0].entity += 1ull << 32;
    CHECK(update(tracker, records).regions.size() == 2);
    unchanged(tracker, records);
    tracker.clear();
    CHECK(tracker.size() == 0);
    CHECK(update(tracker, records).regions.size() == 1);
}

TEST_CASE("shadow caster mesh subranges stay independent for one entity") {
    ShadowCasterTracker tracker;
    std::array records{caster(), caster()};
    records[1].first_index = 36;
    records[1].index_count = 12;
    CHECK(update(tracker, records).regions.size() == 2);
    CHECK(tracker.size() == 2);
    std::swap(records[0], records[1]);
    unchanged(tracker, records);
    records[0].mesh += 1;
    CHECK(update(tracker, records).regions.size() == 2);
    unchanged(tracker, records);
    records[0].index_count += 3;
    CHECK(update(tracker, records).regions.size() == 2);
    unchanged(tracker, records);
    auto removed = update(tracker, std::span(records).first(1));
    CHECK(removed.regions.size() == 1);
    CHECK(tracker.size() == 1);
}

TEST_CASE("shadow caster mask alpha texture generation cull and UV state affect depth") {
    ShadowCasterTracker tracker;
    std::array records{caster()};
    auto& record = records[0];
    record.flags = render::DrawItem::AlphaMasked;
    record.alpha_cutoff = 0.4f;
    record.base_color_texture = {9, 1};
    update(tracker, records);
    unchanged(tracker, records);
    record.alpha_factor = 0.5f;
    CHECK(update(tracker, records).regions.size() == 2);
    record.alpha_cutoff = 0.6f;
    CHECK(update(tracker, records).regions.size() == 2);
    record.base_color_texture.generation += 1;
    CHECK(update(tracker, records).regions.size() == 2);
    record.base_color_texture.index += 1;
    CHECK(update(tracker, records).regions.size() == 2);
    record.flags |= render::DrawItem::ClampUv;
    CHECK(update(tracker, records).regions.size() == 2);
    record.flags |= render::DrawItem::DoubleSided;
    CHECK(update(tracker, records).regions.size() == 2);
    unchanged(tracker, records);
    record.flags &= ~render::DrawItem::AlphaMasked;
    CHECK(update(tracker, records).regions.size() == 2);
    unchanged(tracker, records);
}

TEST_CASE("opaque shadow casters ignore cosmetic mask fields and unrelated draw flags") {
    ShadowCasterTracker tracker;
    std::array records{caster()};
    update(tracker, records);
    records[0].base_color_texture = {83, 91};
    records[0].alpha_factor = std::numeric_limits<float>::quiet_NaN();
    records[0].alpha_cutoff = std::numeric_limits<float>::infinity();
    records[0].flags = render::DrawItem::ClampUv | (1u << 31);
    unchanged(tracker, records);
    // RGB, roughness, normals, lighting and material IDs cannot enter this API. Animated opaque
    // water detail therefore cannot invalidate a light that still sees the same caster depth.
    records[0].flags |= render::DrawItem::DoubleSided;
    CHECK(update(tracker, records).regions.size() == 2);
    unchanged(tracker, records);
}

TEST_CASE("deformed shadow casters invalidate every frame and unknown bounds fail globally") {
    ShadowCasterTracker tracker;
    std::array records{caster()};
    records[0].vertex_override = {4, 7};
    CHECK(update(tracker, records).regions.size() == 1);
    for (unsigned frame = 0; frame < 3; ++frame) {
        auto result = update(tracker, records);
        CHECK(result.all == 0);
        CHECK(result.regions.size() == 2);
    }
    records[0].vertex_override_offset = 128;
    CHECK(update(tracker, records).regions.size() == 2);
    records[0].vertex_override.generation += 1;
    CHECK(update(tracker, records).regions.size() == 2);
    records[0].bounds_known = false;
    for (unsigned frame = 0; frame < 3; ++frame) {
        auto result = update(tracker, records);
        CHECK(result.all == 1);
        CHECK(result.regions.empty());
    }
    CHECK(update(tracker, {}).all == 1);
    unchanged(tracker, {});
    records[0] = caster();
    records[0].dynamic = true; // a caller may know deformation without an override buffer
    update(tracker, records);
    CHECK(update(tracker, records).regions.size() == 2);
}

TEST_CASE("nonfinite model bounds masked alpha and inverted bounds fail conservatively") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::array invalid{caster(), caster(), caster(), caster(), caster()};
    invalid[0].model.m[4] = nan;
    invalid[1].bounds.min.z = inf;
    invalid[2].bounds.max.x = -1;
    invalid[3].flags = render::DrawItem::AlphaMasked;
    invalid[3].alpha_factor = nan;
    invalid[4].flags = render::DrawItem::AlphaMasked;
    invalid[4].alpha_cutoff = inf;
    for (auto bad : invalid) {
        ShadowCasterTracker tracker;
        std::array records{bad};
        for (unsigned frame = 0; frame < 2; ++frame) {
            auto result = update(tracker, records);
            CHECK(result.all == 1);
            CHECK(result.regions.empty());
        }
        records[0] = caster();
        CHECK(update(tracker, records).all == 1);
        unchanged(tracker, records);
    }
}

TEST_CASE("duplicate shadow caster keys refresh globally through ambiguity and its removal") {
    ShadowCasterTracker tracker;
    std::array records{caster(), caster(1, 8)};
    for (unsigned frame = 0; frame < 3; ++frame) {
        std::swap(records[0], records[1]);
        const auto result = update(tracker, records);
        CHECK(result.all == 1);
        CHECK(result.regions.empty());
        CHECK(tracker.size() == 1);
    }
    CHECK(update(tracker, std::span(records).first(1)).all == 1);
    unchanged(tracker, std::span(records).first(1));
    CHECK(update(tracker, records).all == 1);
    CHECK(update(tracker, {}).all == 1);
    unchanged(tracker, {});
    // Even identical duplicate records are ambiguous identities: never assume which row will
    // survive a later topology/material edit merely because today's fields happened to match.
    records[1] = records[0];
    CHECK(update(tracker, records).all == 1);
}
