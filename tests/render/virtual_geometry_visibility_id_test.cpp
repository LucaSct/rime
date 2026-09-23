// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <doctest/doctest.h>

#include "rime/render/virtual_geometry_visibility_id.hpp"

using namespace rime;

TEST_CASE("virtual geometry visibility id: checked, versioned R32Uint packing (M18)") {
    using render::kInvalidVirtualGeometryVisibilityId;
    using render::kVirtualGeometryVisibilityMaxCluster;
    using render::kVirtualGeometryVisibilityMaxGeneration;
    using render::kVirtualGeometryVisibilityMaxVersion;
    using render::pack_virtual_geometry_visibility_id;
    using render::unpack_virtual_geometry_visibility_id;
    using render::VirtualGeometryVisibilityId;

    SUBCASE("all allocated v1 fields round-trip at their boundaries") {
        const VirtualGeometryVisibilityId id{
            kVirtualGeometryVisibilityMaxCluster, kVirtualGeometryVisibilityMaxGeneration, 1};
        const auto packed = pack_virtual_geometry_visibility_id(id);
        REQUIRE(packed.has_value());
        CHECK(*packed != kInvalidVirtualGeometryVisibilityId);
        CHECK(unpack_virtual_geometry_visibility_id(*packed) == id);
    }

    SUBCASE("all allocated v2 fields round-trip at their boundaries, triangle included") {
        const VirtualGeometryVisibilityId id{render::kVirtualGeometryVisibilityV2MaxCluster,
                                             render::kVirtualGeometryVisibilityV2MaxGeneration,
                                             2,
                                             render::kVirtualGeometryVisibilityV2MaxTriangle};
        const auto packed = pack_virtual_geometry_visibility_id(id);
        REQUIRE(packed.has_value());
        CHECK(*packed == 0x2fffffffu); // every non-version bit is allocated
        CHECK(unpack_virtual_geometry_visibility_id(*packed) == id);
        // Fields are independent: the triangle lives in the low bits, below the slot.
        CHECK(*pack_virtual_geometry_visibility_id({1, 0, 2, 0}) == 0x20000080u);
        CHECK(*pack_virtual_geometry_visibility_id({0, 0, 2, 5}) == 0x20000005u);
    }

    SUBCASE("version 1 is frozen: the same bits still decode to the step-1 layout") {
        // 0x1030_0007 was written by step 1 for slot 7, generation 3; v2 must not reinterpret it.
        const auto v1 = unpack_virtual_geometry_visibility_id(0x10300007u);
        REQUIRE(v1.has_value());
        CHECK(*v1 == VirtualGeometryVisibilityId{7, 3, 1, 0});
        CHECK_FALSE(pack_virtual_geometry_visibility_id({7, 3, 1, 1})); // v1 has no triangle
    }

    SUBCASE("the default identity is the current version") {
        CHECK(VirtualGeometryVisibilityId{}.version ==
              render::kVirtualGeometryVisibilityCurrentVersion);
    }

    SUBCASE("zero is the empty sentinel and is never a valid decoded identity") {
        CHECK_FALSE(unpack_virtual_geometry_visibility_id(kInvalidVirtualGeometryVisibilityId));
        CHECK_FALSE(pack_virtual_geometry_visibility_id({0, 0, 0}));
    }

    SUBCASE("overflow and reserved versions are rejected") {
        CHECK_FALSE(
            pack_virtual_geometry_visibility_id({kVirtualGeometryVisibilityMaxCluster + 1, 0, 1}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id(
            {0, kVirtualGeometryVisibilityMaxGeneration + 1, 1}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id({0, 0, 0}));
        CHECK_FALSE(
            pack_virtual_geometry_visibility_id({0, 0, kVirtualGeometryVisibilityMaxVersion + 1}));
        CHECK_FALSE(unpack_virtual_geometry_visibility_id(0x0f000000u));
        // v2 bounds are narrower than v1's.
        CHECK_FALSE(pack_virtual_geometry_visibility_id(
            {render::kVirtualGeometryVisibilityV2MaxCluster + 1, 0, 2}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id(
            {0, render::kVirtualGeometryVisibilityV2MaxGeneration + 1, 2}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id(
            {0, 0, 2, render::kVirtualGeometryVisibilityV2MaxTriangle + 1}));
        // Versions with no layout in this build are rejected, never decoded as v1.
        CHECK_FALSE(pack_virtual_geometry_visibility_id({1, 0, 3}));
        CHECK_FALSE(
            pack_virtual_geometry_visibility_id({1, 0, kVirtualGeometryVisibilityMaxVersion}));
        CHECK_FALSE(unpack_virtual_geometry_visibility_id(0x30000001u));
        CHECK_FALSE(unpack_virtual_geometry_visibility_id(0xf0000001u));
    }

    SUBCASE("recycled slots do not alias stale generations") {
        const auto old_id = pack_virtual_geometry_visibility_id({17, 4, 2});
        const auto new_id = pack_virtual_geometry_visibility_id({17, 5, 2});
        REQUIRE(old_id.has_value());
        REQUIRE(new_id.has_value());
        CHECK(*old_id != *new_id);
        CHECK(unpack_virtual_geometry_visibility_id(*old_id)->generation == 4);
        CHECK(unpack_virtual_geometry_visibility_id(*new_id)->generation == 5);
    }
}
