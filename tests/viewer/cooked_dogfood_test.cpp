// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M6.6 dogfood proof (ADR-0016 rules 3+4): a binary STL cooked by the offline pipeline
// (tools/asset-pipeline) loads back through engine/assets and reproduces *exactly* the vertex soup
// the ICEM viewer's own in-process STL loader builds live. This is the graduation of the loader
// *pattern* into the offline pipeline — the proof that the cooked path is not glTF-shaped: an STL
// cooks to the very same RMA1 MeshAsset the engine already loads and renders.
//
// "Same geometry, two data paths." No GPU is needed: an indexed mesh that index-expands to the
// identical soup renders identically under one shader *by construction*, so this CPU equality is a
// stronger, fully-deterministic check than a pixel diff — and it fits this repo's GPU-free proof
// discipline (the tests/viewer pattern is explicitly device-free). Positions are bit-exact (both
// loaders only read the file's f32s — no arithmetic); normals are computed by the identical
// cross-product and scale-independent normalize, so they agree to within a sub-ULP margin (the only
// slack is cross-language floating-point contraction, which an 8-bit render never resolves anyway).
//
// cube.stl → cube.rmesh is the committed cross-language fixture (tests/assets/fixtures/README.md);
// regenerate with `rime cook tests/assets/fixtures/cube.stl --out <tmp>` and copy the .rmesh back.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/mesh_asset.hpp"
#include "rime/platform/filesystem.hpp"
#include "stl.hpp" // the viewer's live loader (samples/03-icem-viewer, on this test's include path)

using rime::assets::read_mesh;

namespace {
std::filesystem::path fixtures() {
    return std::filesystem::path(RIME_ASSETS_FIXTURE_DIR);
}
} // namespace

TEST_CASE("M6.6 dogfood: a cooked STL reproduces the viewer's live-loaded soup") {
    // Live path: the viewer's own loader, straight from the .stl bytes — the reference soup.
    const auto live = rime::viewer::load_stl_binary_file((fixtures() / "cube.stl").string());
    REQUIRE(live.has_value());
    const std::size_t soup = live->vertices.size();
    CHECK(soup == 36); // a cube: 12 triangles x 3 vertices, un-indexed

    // Cooked path: the same STL, offline-cooked to RMA1 and read back through engine/assets.
    const auto file = rime::platform::read_file(fixtures() / "cube.rmesh");
    REQUIRE(file.has_value());
    rime::assets::AssetError err{};
    const auto cooked = read_mesh(*file, err);
    REQUIRE(cooked.has_value());

    // It is the v1 position/normal/uv layout the engine renders, deduped into an index buffer.
    CHECK(cooked->attribs == rime::assets::kMeshV1Attribs);
    CHECK(cooked->vertex_stride == 32u);
    CHECK(cooked->indices.size() == soup); // one index per original soup vertex
    CHECK(cooked->vertex_count < soup);    // dedup merged the shared coplanar corners
    CHECK(cooked->vertex_count == 24u);    // cube: 6 faces x 4 unique corners each

    // Index-expand the cooked mesh and prove each soup vertex matches the live loader's, in order —
    // the property that makes the two data paths pixel-identical.
    for (std::size_t i = 0; i < soup; ++i) {
        const std::uint32_t vi = cooked->indices[i];
        REQUIRE(vi < cooked->vertex_count);
        const std::byte* base =
            cooked->vertices.data() + static_cast<std::size_t>(vi) * cooked->vertex_stride;
        float p[3];
        float n[3];
        std::memcpy(p, base, sizeof p);
        std::memcpy(n, base + sizeof p, sizeof n);

        const rime::viewer::MeshVertex& lv = live->vertices[i];
        // Positions are pure file reads on both sides → bit-identical.
        CHECK(p[0] == lv.px);
        CHECK(p[1] == lv.py);
        CHECK(p[2] == lv.pz);
        // Normals share the exact formula; a sub-ULP margin absorbs cross-language FP contraction.
        CHECK(n[0] == doctest::Approx(lv.nx).epsilon(1e-6));
        CHECK(n[1] == doctest::Approx(lv.ny).epsilon(1e-6));
        CHECK(n[2] == doctest::Approx(lv.nz).epsilon(1e-6));
    }
}
