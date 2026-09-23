// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <doctest/doctest.h>

#include "rime/render/virtual_geometry_visibility_id.hpp"

using namespace rime;
using namespace rime::render;

TEST_CASE("virtual geometry visibility id v1: frozen 32-bit packing (M18)") {
    SUBCASE("all allocated fields round-trip at their boundaries") {
        const VirtualGeometryVisibilityId id{
            kVirtualGeometryVisibilityMaxCluster, kVirtualGeometryVisibilityMaxGeneration, 1};
        const auto packed = pack_virtual_geometry_visibility_id(id);
        REQUIRE(packed.has_value());
        CHECK(*packed == 0x1fffffffu);
        CHECK(unpack_virtual_geometry_visibility_id(*packed) == id);
    }

    SUBCASE("the step-1 bit pattern still decodes to the step-1 identity") {
        CHECK(*unpack_virtual_geometry_visibility_id(0x10300007u) ==
              VirtualGeometryVisibilityId{7, 3, 1, 0});
    }

    SUBCASE("zero, overflow, a triangle, and other versions are rejected") {
        CHECK_FALSE(unpack_virtual_geometry_visibility_id(kInvalidVirtualGeometryVisibilityId));
        CHECK_FALSE(
            pack_virtual_geometry_visibility_id({kVirtualGeometryVisibilityMaxCluster + 1, 0, 1}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id(
            {0, kVirtualGeometryVisibilityMaxGeneration + 1, 1}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id({7, 3, 1, 1}));
        for (std::uint32_t v : {0u, 2u, 3u, kVirtualGeometryVisibilityMaxVersion}) {
            CHECK_FALSE(pack_virtual_geometry_visibility_id({1, 0, v}));
            CHECK_FALSE(unpack_virtual_geometry_visibility_id((v << 28) | 1u));
        }
    }
}

TEST_CASE("virtual geometry visibility id v3: checked 64-bit packing (M18.2)") {
    SUBCASE("the default identity is the current version, v3") {
        CHECK(VirtualGeometryVisibilityId{}.version == kVirtualGeometryVisibilityCurrentVersion);
        CHECK(kVirtualGeometryVisibilityCurrentVersion == 3u);
    }

    SUBCASE("all allocated fields round-trip at their boundaries") {
        const VirtualGeometryVisibilityId max{kVirtualGeometryVisibilityV3MaxCluster,
                                              kVirtualGeometryVisibilityV3MaxGeneration,
                                              3,
                                              kVirtualGeometryVisibilityV3MaxTriangle};
        const auto packed = pack_virtual_geometry_visibility_id64(max);
        REQUIRE(packed.has_value());
        CHECK(*packed == VirtualGeometryVisibilityWords{0xffffffffu, 0x3fffffffu});
        CHECK(unpack_virtual_geometry_visibility_id64(*packed) == max);

        const VirtualGeometryVisibilityId min{0, 0, 3, 0};
        CHECK(*pack_virtual_geometry_visibility_id64(min) ==
              VirtualGeometryVisibilityWords{0u, 0x30000000u});
        CHECK(unpack_virtual_geometry_visibility_id64(
                  *pack_virtual_geometry_visibility_id64(min)) == min);
    }

    SUBCASE("fields are independent and land in their stated bits") {
        CHECK(*pack_virtual_geometry_visibility_id64({0, 0, 3, 5}) ==
              VirtualGeometryVisibilityWords{5u, 0x30000000u});
        CHECK(*pack_virtual_geometry_visibility_id64({1, 0, 3, 0}) ==
              VirtualGeometryVisibilityWords{0x80u, 0x30000000u});
        CHECK(*pack_virtual_geometry_visibility_id64({0, 1, 3, 0}) ==
              VirtualGeometryVisibilityWords{0u, 0x30000001u});
        // The slot that v2 could not hold.
        const VirtualGeometryVisibilityId wide{65536, 0, 3, 0};
        CHECK(unpack_virtual_geometry_visibility_id64(
                  *pack_virtual_geometry_visibility_id64(wide)) == wide);
    }

    SUBCASE("zero is the empty sentinel; overflow and other versions are rejected") {
        CHECK_FALSE(
            unpack_virtual_geometry_visibility_id64(kInvalidVirtualGeometryVisibilityWords));
        CHECK_FALSE(unpack_virtual_geometry_visibility_id64({5u, 0u}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id64(
            {kVirtualGeometryVisibilityV3MaxCluster + 1, 0, 3}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id64(
            {0, kVirtualGeometryVisibilityV3MaxGeneration + 1, 3}));
        CHECK_FALSE(pack_virtual_geometry_visibility_id64(
            {0, 0, 3, kVirtualGeometryVisibilityV3MaxTriangle + 1}));
        for (std::uint32_t v : {0u, 1u, 2u, 4u, kVirtualGeometryVisibilityMaxVersion}) {
            CHECK_FALSE(pack_virtual_geometry_visibility_id64({1, 0, v}));
            CHECK_FALSE(unpack_virtual_geometry_visibility_id64({1u, v << 28}));
        }
    }

    SUBCASE("recycled slots do not alias stale generations") {
        const auto a = *pack_virtual_geometry_visibility_id64({17, 4});
        const auto b = *pack_virtual_geometry_visibility_id64({17, 5});
        CHECK_FALSE(a == b);
        CHECK(unpack_virtual_geometry_visibility_id64(a)->generation == 4);
        CHECK(unpack_virtual_geometry_visibility_id64(b)->generation == 5);
    }
}
