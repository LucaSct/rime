// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the viewer's binary-STL loader (part of B1). It builds binary STLs in memory and checks
// the parse: triangle/vertex counts, bounding box, and — the one that bit us — that face normals
// come out unit length *regardless of scale*. A sub-millimetre triangle (a finely-meshed
// metre-scale ICEM part) has a cross-product magnitude ~1e-8; a scale-blind normalize would null it
// to zero and the surface would shade black, so this pins the scale-independent normalization. No
// GPU needed.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "stl.hpp" // samples/03-icem-viewer, on the test's include path (see CMakeLists.txt)

using rime::viewer::CpuMesh;
using rime::viewer::load_stl_binary;
using rime::viewer::make_unit_cube;

namespace {

void put_f(std::vector<std::byte>& b, float f) {
    std::byte t[4];
    std::memcpy(t, &f, 4);
    b.insert(b.end(), t, t + 4);
}

void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    std::byte t[4];
    std::memcpy(t, &v, 4);
    b.insert(b.end(), t, t + 4);
}

// Build a binary STL: 80-byte header, triangle count, then per-triangle (normal, v0, v1, v2, attr).
// The stored normal is left zero so the loader must derive it from the geometry.
std::vector<std::byte> build_stl(const std::vector<std::array<float, 9>>& tris) {
    std::vector<std::byte> b;
    for (int i = 0; i < 80; ++i)
        b.push_back(std::byte{0}); // header
    put_u32(b, static_cast<std::uint32_t>(tris.size()));
    for (const auto& t : tris) {
        put_f(b, 0.0f); // stored normal x/y/z = 0 → force geometric derivation
        put_f(b, 0.0f);
        put_f(b, 0.0f);
        for (float v : t)
            put_f(b, v);           // v0, v1, v2 (9 floats)
        b.push_back(std::byte{0}); // attribute byte count (uint16) = 0
        b.push_back(std::byte{0});
    }
    return b;
}

float normal_len(const rime::viewer::MeshVertex& v) {
    return std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
}

} // namespace

TEST_CASE("stl loader: counts, bounds, and a unit-scale normal") {
    // One triangle in the z=0 plane: geometric normal is +z.
    const auto bytes = build_stl({{{0, 0, 0, 1, 0, 0, 0, 1, 0}}});
    const auto mesh = load_stl_binary(bytes.data(), bytes.size());
    REQUIRE(mesh.has_value());
    CHECK(mesh->triangle_count() == 1);
    CHECK(mesh->vertices.size() == 3);

    CHECK(mesh->bb_min.x == doctest::Approx(0.0f));
    CHECK(mesh->bb_max.x == doctest::Approx(1.0f));
    CHECK(mesh->bb_max.y == doctest::Approx(1.0f));

    for (const auto& v : mesh->vertices) {
        CHECK(normal_len(v) == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(v.nz == doctest::Approx(1.0f).epsilon(0.001)); // +z face
    }
}

TEST_CASE("stl loader: a sub-millimetre triangle still yields a unit normal (scale independence)") {
    // The regression: a ~1e-4-sized triangle (cross-product magnitude ~1e-8). A scale-blind
    // normalize would return zero here and the surface would shade black.
    const float s = 1.0e-4f;
    const auto bytes = build_stl({{{0, 0, 0, s, 0, 0, 0, s, 0}}});
    const auto mesh = load_stl_binary(bytes.data(), bytes.size());
    REQUIRE(mesh.has_value());
    REQUIRE(mesh->vertices.size() == 3);
    for (const auto& v : mesh->vertices) {
        CHECK(normal_len(v) == doctest::Approx(1.0f).epsilon(0.001)); // unit, not zero
        CHECK(v.nz == doctest::Approx(1.0f).epsilon(0.001));
    }
}

TEST_CASE("stl loader: rejects malformed buffers") {
    CHECK_FALSE(load_stl_binary(nullptr, 0).has_value());
    std::vector<std::byte> tiny(10, std::byte{0});
    CHECK_FALSE(load_stl_binary(tiny.data(), tiny.size()).has_value()); // too small for a header
    const auto empty = build_stl({});
    CHECK_FALSE(load_stl_binary(empty.data(), empty.size()).has_value()); // zero triangles
}

TEST_CASE("stl loader: unit cube fixture is well-formed with unit normals") {
    const CpuMesh cube = make_unit_cube();
    CHECK(cube.triangle_count() == 12);
    CHECK(cube.vertices.size() == 36);
    CHECK(cube.bb_min.x == doctest::Approx(-1.0f));
    CHECK(cube.bb_max.z == doctest::Approx(1.0f));
    CHECK(cube.radius() == doctest::Approx(0.5f * std::sqrt(12.0f))); // half-diagonal of a 2³ box
    for (const auto& v : cube.vertices) {
        CHECK(normal_len(v) == doctest::Approx(1.0f).epsilon(0.001));
    }
}
