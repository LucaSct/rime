// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The M6.2 cross-language proof: a mesh cooked by the Rust `tools/asset-pipeline` (the committed
// tests/assets/fixtures/quad.rmesh) loads through this C++ reader and decodes to exactly the values
// the source glTF describes. The Rust side's cook_fixture.rs proves the cooker still produces these
// bytes; this side proves the reader ingests them — together, the two languages cannot drift on the
// RMA1 format without a red test. RIME_ASSETS_FIXTURE_DIR is injected by CMake.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/hash.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/platform/filesystem.hpp"

using namespace rime::assets;

namespace {
// Read a float out of the interleaved vertex blob at a byte offset (memcpy, so no
// aliasing/alignment UB — the negative-battery suite runs under ASan/UBSan).
float blob_f32(const std::vector<std::byte>& blob, std::size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, blob.data() + offset, sizeof(value));
    return value;
}

std::optional<std::vector<std::byte>> load_fixture(const char* name) {
    return rime::platform::read_file(std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / name);
}

// A palette entry from a rigid skeleton (no scale) must be finite and have an orthonormal
// upper-left 3×3 — the columns unit-length and mutually perpendicular. This is the structural
// invariant the RiggedSimple-style proof checks: whatever the pose, the rotation part stays a
// rotation. Column c, row r of the column-major matrix is m[c*4 + r].
void check_finite_orthonormal(const rime::core::Mat4& m) {
    for (const float v : m.m) {
        REQUIRE(std::isfinite(v));
    }
    const std::array<std::array<float, 3>, 3> cols = {
        {{m.m[0], m.m[1], m.m[2]}, {m.m[4], m.m[5], m.m[6]}, {m.m[8], m.m[9], m.m[10]}}};
    const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    for (int i = 0; i < 3; ++i) {
        CHECK(std::sqrt(dot(cols[i], cols[i])) == doctest::Approx(1.0f).epsilon(0.001));
        for (int j = i + 1; j < 3; ++j) {
            CHECK(std::abs(dot(cols[i], cols[j])) < 0.001f);
        }
    }
}
} // namespace

TEST_CASE("a Rust-cooked glTF mesh (quad.rmesh) loads and matches its known values") {
    const std::filesystem::path path =
        std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / "quad.rmesh";
    const std::optional<std::vector<std::byte>> bytes = rime::platform::read_file(path);
    REQUIRE_MESSAGE(bytes.has_value(), "missing fixture: ", path.string());

    AssetError error{};
    AssetId id;
    const std::optional<MeshAsset> mesh = read_mesh(*bytes, error, &id);
    REQUIRE(mesh.has_value());

    // The quad: 4 vertices, 6 indices, position|normal|uv, one submesh.
    CHECK(mesh->attribs == kMeshV1Attribs);
    CHECK(mesh->vertex_stride == 32);
    CHECK(mesh->vertex_count == 4);
    CHECK(mesh->indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    REQUIRE(mesh->submeshes.size() == 1);
    CHECK(mesh->submeshes[0].first_index == 0);
    CHECK(mesh->submeshes[0].index_count == 6);
    CHECK(id.is_valid());

    // The source node is translated by (1,2,3); the cooker flattened it into the vertices, so the
    // bounds and the first vertex are offset by it.
    CHECK(mesh->bounds.min.x == doctest::Approx(1.0f));
    CHECK(mesh->bounds.min.y == doctest::Approx(2.0f));
    CHECK(mesh->bounds.min.z == doctest::Approx(3.0f));
    CHECK(mesh->bounds.max.x == doctest::Approx(2.0f));
    CHECK(mesh->bounds.max.y == doctest::Approx(3.0f));

    // Vertex 0: local (0,0,0) → world (1,2,3), normal +Z, uv (0,0). Layout: px,py,pz,nx,ny,nz,u,v.
    REQUIRE(mesh->vertices.size() == 4u * 32u);
    CHECK(blob_f32(mesh->vertices, 0) == doctest::Approx(1.0f));  // px
    CHECK(blob_f32(mesh->vertices, 4) == doctest::Approx(2.0f));  // py
    CHECK(blob_f32(mesh->vertices, 8) == doctest::Approx(3.0f));  // pz
    CHECK(blob_f32(mesh->vertices, 20) == doctest::Approx(1.0f)); // nz
    CHECK(blob_f32(mesh->vertices, 24) == doctest::Approx(0.0f)); // u
}

TEST_CASE("a Rust-cooked PNG texture (checker.rtex) loads with a gamma-correct mip chain") {
    // The M6.3 texture cross-language proof: a 2×2 sRGB checker cooked by the Rust pipeline (the
    // committed checker.rtex) loads through this reader with the right extent, format, mip table,
    // and pixels — and its 1×1 mip proves the cooker generated the chain gamma-correctly.
    // cook_fixture.rs proves the cooker still emits these bytes; this proves the reader ingests
    // them.
    const std::filesystem::path path =
        std::filesystem::path(RIME_ASSETS_FIXTURE_DIR) / "checker.rtex";
    const std::optional<std::vector<std::byte>> bytes = rime::platform::read_file(path);
    REQUIRE_MESSAGE(bytes.has_value(), "missing fixture: ", path.string());

    AssetError error{};
    AssetId id;
    const std::optional<TextureAsset> tex = read_texture(*bytes, error, &id);
    REQUIRE_MESSAGE(tex.has_value(), to_string(error));

    CHECK(tex->width == 2);
    CHECK(tex->height == 2);
    CHECK(tex->format == TextureFormat::Rgba8Srgb); // sRGB is the cook default for colour
    REQUIRE(tex->mips.size() == 2);                 // 2×2 → 1×1
    CHECK(tex->mips[0].size == 16);
    CHECK(tex->mips[1].width == 1);
    CHECK(tex->mips[1].offset == 16);
    CHECK(tex->mips[1].size == 4);
    CHECK(id.is_valid());

    // Level 0 survived verbatim, top row first: the top-left texel is white. This pins the row
    // order cross-language — a vertically-flipped cook would put black here.
    REQUIRE(tex->pixels.size() == 20);
    CHECK(tex->pixels[0] == std::byte{255}); // top-left R (white)
    CHECK(tex->pixels[1] == std::byte{255});
    CHECK(tex->pixels[2] == std::byte{255});
    CHECK(tex->pixels[4] == std::byte{0}); // top-right R (black)

    // The 1×1 mip is the *gamma-correct* average of two black + two white texels: linear 0.5 re-
    // encoded to sRGB 188 — proof the Rust cooker filtered in linear space, not the too-dark naive
    // byte average (which would be 128). This is the brick's headline behaviour, checked across the
    // language boundary from the committed file.
    const std::size_t m1 = tex->mips[1].offset;
    CHECK(tex->pixels[m1 + 0] == std::byte{188});
    CHECK(tex->pixels[m1 + 1] == std::byte{188});
    CHECK(tex->pixels[m1 + 2] == std::byte{188});
    CHECK(tex->pixels[m1 + 3] == std::byte{255}); // alpha is linear: 255 stays 255
}

TEST_CASE("a Rust-cooked glTF skeleton (skinned.rskel) loads, reordered parent-first") {
    // The M6.7 skeletal cross-language proof. The source skinned.gltf lists its skin joints
    // CHILD-FIRST (B before its parent A) on purpose; the cooker reorders them into topological
    // order, so the mere fact that this reader ACCEPTS the file (it rejects a non-topological one
    // with InvalidSkeleton) is proof the reorder ran. cook_fixture.rs proves the cooker still emits
    // these exact bytes; this proves the reader ingests them.
    const std::optional<std::vector<std::byte>> bytes = load_fixture("skinned.rskel");
    REQUIRE_MESSAGE(bytes.has_value(), "missing fixture: skinned.rskel");

    AssetError error{};
    AssetId id;
    const std::optional<Skeleton> sk = read_skeleton(*bytes, error, &id);
    REQUIRE_MESSAGE(sk.has_value(), to_string(error));
    CHECK(id.is_valid());

    REQUIRE(sk->joint_count() == 2);
    CHECK(sk->is_topologically_ordered()); // the reorder normalized the child-first skin

    // Name lookup crosses the language boundary: the cooker hashed "A"/"B" with FNV-1a, and this
    // reader's core::fnv1a_64 of the same names finds them — so root A landed at index 0, child B
    // at 1.
    CHECK(sk->find(rime::core::fnv1a_64(std::string_view("A"))) == 0);
    CHECK(sk->find(rime::core::fnv1a_64(std::string_view("B"))) == 1);
    CHECK(sk->joints[0].parent == Joint::kNoParent); // A is the root
    CHECK(sk->joints[1].parent == 0);                // B's parent is A

    // Bind poses: A at the origin, B two units along +X; B's inverse bind undoes that (-2 on X).
    CHECK(sk->joints[0].local_bind.translation.x == doctest::Approx(0.0f));
    CHECK(sk->joints[1].local_bind.translation.x == doctest::Approx(2.0f));
    CHECK(sk->joints[0].inverse_bind.m[12] == doctest::Approx(0.0f));
    CHECK(sk->joints[1].inverse_bind.m[12] == doctest::Approx(-2.0f));
}

TEST_CASE("a Rust-cooked glTF clip (skinned.Spin.ranim) loads and samples to a valid palette") {
    // The M6.7 clip cross-language proof: the "Spin" animation cooked from skinned.gltf loads, its
    // channels reconstruct into the right joints, and sampling it against the skeleton yields a
    // finite, orthonormal palette — the import→cook→load→sample path proven end to end across the
    // boundary.
    const std::optional<std::vector<std::byte>> skel_bytes = load_fixture("skinned.rskel");
    const std::optional<std::vector<std::byte>> clip_bytes = load_fixture("skinned.Spin.ranim");
    REQUIRE(skel_bytes.has_value());
    REQUIRE_MESSAGE(clip_bytes.has_value(), "missing fixture: skinned.Spin.ranim");

    AssetError error{};
    const std::optional<Skeleton> sk = read_skeleton(*skel_bytes, error);
    REQUIRE_MESSAGE(sk.has_value(), to_string(error));
    const std::optional<Clip> clip = read_clip(*clip_bytes, error);
    REQUIRE_MESSAGE(clip.has_value(), to_string(error));

    CHECK(clip->duration == doctest::Approx(1.0f));
    REQUIRE(clip->joint_count() == 2);
    // Root A (joint 0) slides on a translation track; child B (joint 1) spins on a rotation track.
    CHECK_FALSE(clip->joints[0].translation.empty());
    CHECK(clip->joints[0].rotation.empty());
    CHECK_FALSE(clip->joints[1].rotation.empty());
    CHECK(clip->joints[1].translation.empty());

    std::array<rime::core::Mat4, 2> palette{};

    // At t=0 the first keys reproduce the bind pose, so the palette is identity for every joint —
    // the same bind-pose invariant the hand-built sampler test pins, here from an imported clip.
    REQUIRE(sample_clip(*clip, *sk, 0.0f, TimePolicy::Clamp, palette) == 2);
    const rime::core::Mat4 identity{};
    for (const rime::core::Mat4& m : palette) {
        for (int i = 0; i < 16; ++i) {
            CHECK(m.m[i] == doctest::Approx(identity.m[i]).epsilon(0.001));
        }
    }

    // At t=1 root A has slid to +1 on Y; its palette (inverse bind is identity) is that
    // translation.
    REQUIRE(sample_clip(*clip, *sk, 1.0f, TimePolicy::Clamp, palette) == 2);
    CHECK(palette[0].m[13] == doctest::Approx(1.0f)); // translation Y column

    // At every sampled time each palette entry is finite with an orthonormal rotation part (the rig
    // is rigid, so no pose can shear it) — the structural proof, checked across the mid-animation
    // pose too.
    for (const float t : {0.0f, 0.5f, 1.0f}) {
        REQUIRE(sample_clip(*clip, *sk, t, TimePolicy::Clamp, palette) == 2);
        for (const rime::core::Mat4& m : palette) {
            check_finite_orthonormal(m);
        }
    }
}
