// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include "character_fixture.hpp"
#include "scar_reanchor.hpp"

using namespace rime;
using rime_test::add_static_box;

namespace {

bool probe(const physics::PhysicsWorld& w,
           core::Vec3 p,
           core::Vec3 n,
           block_play::ScarProbeHit& out) {
    return block_play::find_scar_anchor(
        p,
        n,
        [&](core::Vec3 origin, core::Vec3 dir, float reach, block_play::ScarProbeHit& hit) {
            physics::RayHit r{};
            if (!w.raycast({origin, dir, reach}, r))
                return false;
            hit = {r.body, r.point};
            return true;
        },
        out);
}

} // namespace

TEST_CASE(
    "a scar re-anchors to the piece that still holds its spot, and drops when the piece left") {
    physics::PhysicsWorld w;
    // A wall slab, front face at z = +0.1; two scars on that face.
    const auto wall = add_static_box(w, {1.0f, 1.0f, 0.1f}, {0, 1, 0});
    const core::Vec3 stay{0.5f, 1.0f, 0.1f}, leave{-0.5f, 1.0f, 0.1f}, n{0, 0, 1};
    block_play::ScarProbeHit hit{};
    REQUIRE(probe(w, stay, n, hit));
    CHECK(hit.body == wall); // sanity: before fracture the probe finds the original body

    // "Fracture": the slab is replaced by a right half that stays and a left half that has
    // moved away (debris). The old BodyId is stale.
    w.destroy_body(wall);
    const auto kept = add_static_box(w, {0.5f, 1.0f, 0.1f}, {0.5f, 1.0f, 0.0f});
    const auto moved = add_static_box(w, {0.5f, 1.0f, 0.1f}, {-0.5f, 6.0f, 0.0f});

    SUBCASE("the scar on the standing piece lands on that piece") {
        REQUIRE(probe(w, stay, n, hit));
        CHECK(hit.body == kept);
    }
    SUBCASE("the scar whose piece left finds nothing and is dropped") {
        CHECK_FALSE(probe(w, leave, n, hit));
    }
    SUBCASE("the debris itself holds the scar at the spot where it now is") {
        // A scar that travelled with the debris is probed at its NEW world pose.
        REQUIRE(probe(w, {-0.5f, 6.0f, 0.1f}, n, hit));
        CHECK(hit.body == moved);
    }
}

TEST_CASE("a hit farther than the tolerance from the old point is not the same surface") {
    // Fake world: a surface 3 cm behind where the scar was (a recessed fracture face).
    const core::Vec3 p{0, 0, 0}, n{0, 0, 1};
    block_play::ScarProbeHit out{};
    auto at = [&](float depth) {
        return [depth](core::Vec3 origin, core::Vec3, float, block_play::ScarProbeHit& h) {
            h.point = {origin.x, origin.y, -depth};
            return true;
        };
    };
    CHECK(block_play::find_scar_anchor(p, n, at(0.01f), out));
    CHECK_FALSE(block_play::find_scar_anchor(p, n, at(0.03f), out));
}
