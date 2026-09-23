// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <doctest/doctest.h>

#include "rime/render/virtual_geometry_visibility_id.hpp"

using namespace rime;

TEST_CASE("virtual geometry visibility id: checked R32Uint packing (M18)") {
    using render::kInvalidVirtualGeometryVisibilityId;
    using render::kVirtualGeometryVisibilityMaxCluster;
    using render::kVirtualGeometryVisibilityMaxGeneration;
    using render::kVirtualGeometryVisibilityMaxVersion;
    using render::pack_virtual_geometry_visibility_id;
    using render::unpack_virtual_geometry_visibility_id;
    using render::VirtualGeometryVisibilityId;

    SUBCASE("all allocated fields round-trip at their boundaries") {
        const VirtualGeometryVisibilityId id{kVirtualGeometryVisibilityMaxCluster,
                                             kVirtualGeometryVisibilityMaxGeneration,
                                             kVirtualGeometryVisibilityMaxVersion};
        const auto packed = pack_virtual_geometry_visibility_id(id);
        REQUIRE(packed.has_value());
        CHECK(*packed != kInvalidVirtualGeometryVisibilityId);
        CHECK(unpack_virtual_geometry_visibility_id(*packed) == id);
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
    }

    SUBCASE("recycled slots do not alias stale generations") {
        const auto old_id = pack_virtual_geometry_visibility_id({17, 4, 1});
        const auto new_id = pack_virtual_geometry_visibility_id({17, 5, 1});
        REQUIRE(old_id.has_value());
        REQUIRE(new_id.has_value());
        CHECK(*old_id != *new_id);
        CHECK(unpack_virtual_geometry_visibility_id(*old_id)->generation == 4);
        CHECK(unpack_virtual_geometry_visibility_id(*new_id)->generation == 5);
    }
}
