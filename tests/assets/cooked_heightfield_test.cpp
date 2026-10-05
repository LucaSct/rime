// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the RMA1 cooked-heightfield reader (M19.1, ADR-0060-m19.1-heightfield). The shape of
// cooked_mesh_sdf_test.cpp, plus the one thing this brick adds to the reader contract — the
// REJECTION COUNTER:
//
//   * SCHEMA HASH — pinned, non-zero, distinct from every other kind's fingerprint.
//   * A HAND-BUILT MINIMAL FILE round-trips, field for field.
//   * NEGATIVE BATTERY — one crafted file per way a heightfield can be wrong (envelope, kind,
//     schema, payload version, every header invariant, the sample blob's size and range), each a
//     clean typed error — AND each tallied by reason in the caller's AssetRejectCounters, so a
//     refusal cannot vanish when the caller falls back and moves on.
//   * TRUNCATION at every byte length fails cleanly and is counted every time.
//
// The Rust-cooked fixture (terrain.rhf) is loaded in fixture_test.cpp with the other cross-language
// proofs. doctest's main() lives in cooked_mesh_test.cpp.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "rime/assets/cooked_reader.hpp"

using namespace rime::assets;

namespace {

void put_u16(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>(v & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}

void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

void put_u64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

void put_f32(std::vector<std::byte>& b, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(b, bits);
}

// The v2 splat-material block (ADR-0063), field by field, so each negative case corrupts one.
struct SplatSpec {
    std::uint32_t weight_columns = 2;
    std::uint32_t weight_rows = 2;
    std::uint32_t layer_count = HeightfieldAsset::kLayerCount;
    std::uint64_t layers[HeightfieldAsset::kLayerCount] = {0x11, 0x22, 0, 0};
    // Four texels: pure layer 0, pure layer 1, a 128/127 split, a 64/191 split. Every one sums to
    // 255, and no weight lands on an unused slot.
    std::vector<std::uint8_t> weights{255, 0, 0, 0, 0, 255, 0, 0, 128, 127, 0, 0, 64, 191, 0, 0};
};

// Every field of a payload, so each negative case corrupts exactly one of them.
struct HfSpec {
    // PINNED TO 1, not kHeightfieldPayloadVersion: these specs describe a payload with no splat
    // block, which is exactly what a v1 payload is. Following the newest version here would have
    // made every case below build a v2 header with no v2 body the moment ADR-0063 landed.
    std::uint32_t version = 1;
    std::uint32_t columns = 3;
    std::uint32_t rows = 2;
    float cell_x = 2.0f;
    float cell_z = 4.0f;
    float ox = 10.0f, oy = -5.0f, oz = 20.0f;
    float scale = 0.5f;
    float offset = -1.0f;
    std::uint32_t triangulation = 0;
    std::uint32_t min_sample = 0;
    std::uint32_t max_sample = 9;
    std::vector<std::uint16_t> samples{0, 3, 9, 1, 2, 4};
    std::optional<SplatSpec> splat{}; // present => a v2 payload; set `version` to 2 as well
};

std::vector<std::byte> payload_of(const HfSpec& s) {
    std::vector<std::byte> p;
    put_u32(p, s.version);
    put_u32(p, s.columns);
    put_u32(p, s.rows);
    put_f32(p, s.cell_x);
    put_f32(p, s.cell_z);
    put_f32(p, s.ox);
    put_f32(p, s.oy);
    put_f32(p, s.oz);
    put_f32(p, s.scale);
    put_f32(p, s.offset);
    put_u32(p, s.triangulation);
    put_u32(p, s.min_sample);
    put_u32(p, s.max_sample);
    for (const std::uint16_t q : s.samples) {
        put_u16(p, q);
    }
    if (s.splat.has_value()) {
        put_u32(p, s.splat->weight_columns);
        put_u32(p, s.splat->weight_rows);
        put_u32(p, s.splat->layer_count);
        for (const std::uint64_t id : s.splat->layers) {
            put_u64(p, id);
        }
        for (const std::uint8_t w : s.splat->weights) {
            p.push_back(static_cast<std::byte>(w));
        }
    }
    return p;
}

// A valid v2 spec: the same grid, plus the splat block above.
HfSpec splat_spec() {
    HfSpec s;
    s.version = 2;
    s.splat = SplatSpec{};
    return s;
}

std::vector<std::byte> wrap(const std::vector<std::byte>& payload,
                            std::uint16_t kind = static_cast<std::uint16_t>(AssetKind::Heightfield),
                            std::uint64_t schema = heightfield_schema_hash(),
                            std::uint16_t container_version = kContainerVersion) {
    std::vector<std::byte> f{std::byte{'R'}, std::byte{'M'}, std::byte{'A'}, std::byte{'1'}};
    put_u16(f, container_version);
    put_u16(f, kind);
    put_u64(f, schema);
    put_u64(f, payload.size());
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// Load `file`, expect refusal with `expected`, and expect the counter to have seen exactly that.
void expect_rejected(const std::vector<std::byte>& file, AssetError expected) {
    AssetRejectCounters rejects;
    AssetError error = AssetError::Io;
    const std::optional<HeightfieldAsset> hf = read_heightfield(file, error, nullptr, &rejects);
    CHECK_FALSE(hf.has_value());
    CHECK_MESSAGE(error == expected, to_string(error));
    CHECK(rejects.total == 1);
    CHECK(rejects.count(expected) == 1);
}

} // namespace

TEST_CASE("heightfield schema hash is pinned and distinct from every other kind") {
    // The value the Rust cooker embeds (cooked.rs HEIGHTFIELD_SCHEMA_HASH). If this changes, the
    // HeightfieldHeaderV1 record changed: update the Rust constant and re-cook terrain.rhf.
    CHECK(heightfield_schema_hash() == 0x94CB7AE5FCC4E8DFull);
    CHECK(heightfield_schema_hash() != 0);
    CHECK(heightfield_schema_hash() != mesh_schema_hash());
    CHECK(heightfield_schema_hash() != texture_schema_hash());
    CHECK(heightfield_schema_hash() != material_schema_hash());
    CHECK(heightfield_schema_hash() != sdf_schema_hash());
    CHECK(heightfield_schema_hash() != destructible_schema_hash());
}

TEST_CASE("a hand-built heightfield round-trips field for field") {
    const HfSpec spec;
    AssetRejectCounters rejects;
    AssetError error = AssetError::Io;
    AssetId id;
    const std::optional<HeightfieldAsset> hf =
        read_heightfield(wrap(payload_of(spec)), error, &id, &rejects);
    REQUIRE_MESSAGE(hf.has_value(), to_string(error));
    CHECK(rejects.total == 0);
    CHECK(id == content_hash(payload_of(spec)));
    CHECK(hf->columns == 3);
    CHECK(hf->rows == 2);
    CHECK(hf->cell_size_x == 2.0f);
    CHECK(hf->cell_size_z == 4.0f);
    CHECK(hf->origin.x == 10.0f);
    CHECK(hf->origin.y == -5.0f);
    CHECK(hf->origin.z == 20.0f);
    CHECK(hf->triangulation == HeightfieldTriangulation::DiagonalMinToMax);
    CHECK(hf->min_sample == 0);
    CHECK(hf->max_sample == 9);
    CHECK(hf->samples == spec.samples);
    // Row-major, x fastest: sample (2, 0) is the third value, (0, 1) the fourth.
    CHECK(hf->height(2, 0) == -1.0f + 0.5f * 9.0f);
    CHECK(hf->height(0, 1) == -1.0f + 0.5f * 1.0f);
}

TEST_CASE("the heightfield reader refuses every malformed file, and counts each refusal") {
    const HfSpec good;

    SUBCASE("envelope: bad magic, container version, wrong kind, schema") {
        std::vector<std::byte> f = wrap(payload_of(good));
        f[0] = std::byte{'X'};
        expect_rejected(f, AssetError::BadMagic);
        expect_rejected(wrap(payload_of(good),
                             static_cast<std::uint16_t>(AssetKind::Heightfield),
                             heightfield_schema_hash(),
                             kContainerVersion + 1),
                        AssetError::UnsupportedVersion);
        expect_rejected(wrap(payload_of(good), static_cast<std::uint16_t>(AssetKind::MeshSdf)),
                        AssetError::WrongKind);
        expect_rejected(wrap(payload_of(good),
                             static_cast<std::uint16_t>(AssetKind::Heightfield),
                             heightfield_schema_hash() ^ 1u),
                        AssetError::SchemaMismatch);
    }
    SUBCASE("payload version from the future (and the past)") {
        HfSpec s = good;
        s.version = kHeightfieldPayloadVersion + 1;
        expect_rejected(wrap(payload_of(s)), AssetError::UnsupportedVersion);
        s.version = 0;
        expect_rejected(wrap(payload_of(s)), AssetError::UnsupportedVersion);
    }
    SUBCASE("grid dimensions: a line is not a surface, and the ceiling holds") {
        HfSpec s = good;
        s.columns = 1;
        s.samples = {0, 1};
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
        s = good;
        s.rows = 0;
        s.samples.clear();
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
        s = good;
        s.columns = 70000; // refused before 70000*rows is ever allocated
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
    }
    SUBCASE("spacing, scale and placement must be finite and positive where it matters") {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        for (const float bad : {0.0f, -1.0f, nan, inf}) {
            HfSpec s = good;
            s.cell_x = bad;
            expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
            s = good;
            s.cell_z = bad;
            expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
            s = good;
            s.scale = bad;
            expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
        }
        HfSpec s = good;
        s.oy = nan;
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
        s = good;
        s.offset = inf;
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
        s = good; // each factor finite, the peak offset + scale * max is not
        s.scale = 3.0e38f;
        s.offset = 3.0e38f;
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
    }
    SUBCASE("an unknown triangulation is refused, not guessed at") {
        HfSpec s = good;
        s.triangulation = 1;
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
    }
    SUBCASE("the sample range must be ordered, u16, and honoured by every sample") {
        HfSpec s = good;
        s.min_sample = 10;
        s.max_sample = 9;
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
        s = good;
        s.max_sample = 0x10000;
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
        s = good;
        s.max_sample = 8; // the blob contains a 9
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
        s = good;
        s.min_sample = 1; // the blob contains a 0
        expect_rejected(wrap(payload_of(s)), AssetError::InvalidHeightfield);
    }
    SUBCASE("the sample blob must be exactly columns * rows u16s") {
        HfSpec s = good;
        s.samples.pop_back();
        expect_rejected(wrap(payload_of(s)), AssetError::SizeMismatch);
        s = good;
        s.samples.push_back(0);
        expect_rejected(wrap(payload_of(s)), AssetError::SizeMismatch);
    }
}

TEST_CASE("a heightfield truncated at every byte length fails cleanly, and every one is counted") {
    const std::vector<std::byte> file = wrap(payload_of(HfSpec{}));
    AssetRejectCounters rejects;
    for (std::size_t len = 0; len < file.size(); ++len) {
        const std::vector<std::byte> cut(file.begin(), file.begin() + static_cast<long>(len));
        AssetError error = AssetError::Io;
        CHECK_FALSE(read_heightfield(cut, error, nullptr, &rejects).has_value());
    }
    // Every cut was refused and tallied: before the header completes it is Truncated; after, the
    // header's payload_size disagrees with the bytes present (SizeMismatch). Nothing uncounted.
    CHECK(rejects.total == file.size());
    CHECK(rejects.count(AssetError::Truncated) + rejects.count(AssetError::SizeMismatch) ==
          file.size());
    CHECK(rejects.count(AssetError::Truncated) == kCookedHeaderSize);

    // And the decoder alone, on a payload cut short at every length (the container envelope can't
    // shield it — a caller may hand it a payload directly).
    const std::vector<std::byte> payload = payload_of(HfSpec{});
    for (std::size_t len = 0; len < payload.size(); ++len) {
        AssetError error = AssetError::Io;
        const std::span<const std::byte> cut(payload.data(), len);
        CHECK_FALSE(decode_heightfield(cut, error).has_value());
        CHECK((error == AssetError::Truncated || error == AssetError::SizeMismatch));
    }
}

TEST_CASE("a v1 heightfield still decodes, and says it has no splat map") {
    // ADR-0063's compatibility promise, and the reason the schema hash did NOT change: every
    // terrain cooked before m19.4 — the cross-language fixture included — must keep reading, and
    // must read as "one material", not as a tile whose weights happen to be missing.
    HfSpec spec; // version 1, no splat block
    AssetError error = AssetError::Io;
    const std::optional<HeightfieldAsset> hf = read_heightfield(wrap(payload_of(spec)), error);
    REQUIRE_MESSAGE(hf.has_value(), to_string(error));
    CHECK_FALSE(hf->has_splat());
    CHECK(hf->weight_columns == 0);
    CHECK(hf->weight_rows == 0);
    CHECK(hf->weights.empty());
    CHECK_FALSE(hf->layers[0].is_valid());
}

TEST_CASE("a v2 heightfield round-trips its palette and its weights as a partition of unity") {
    const HfSpec spec = splat_spec();
    AssetRejectCounters rejects;
    AssetError error = AssetError::Io;
    const std::optional<HeightfieldAsset> hf =
        read_heightfield(wrap(payload_of(spec)), error, nullptr, &rejects);
    REQUIRE_MESSAGE(hf.has_value(), to_string(error));
    CHECK(rejects.total == 0);
    CHECK(hf->has_splat());
    CHECK(hf->weight_columns == 2);
    CHECK(hf->weight_rows == 2);
    CHECK(hf->weight_texel_count() == 4);
    CHECK(hf->layers[0].value == 0x11);
    CHECK(hf->layers[1].value == 0x22);
    CHECK_FALSE(hf->layers[2].is_valid());

    // The heights are untouched by the splat block — the two halves of a v2 payload must not be
    // able to corrupt each other, and a weight blob read one byte off would move the samples too.
    CHECK(hf->samples == spec.samples);

    // Texel (0, 0) is pure layer 0, texel (1, 0) pure layer 1, and the interleave is per texel
    // (x fastest), so a transposed walk would swap these two.
    CHECK(hf->weight(0, 0, 0) == 1.0f);
    CHECK(hf->weight(0, 0, 1) == 0.0f);
    CHECK(hf->weight(1, 0, 1) == 1.0f);

    // THE INVARIANT: every texel's four weights are a partition of unity. Checked on the integers,
    // because that is what the reader validated and what the shader will blend — a float sum of
    // x/255 terms is 1.0f here but need not be for every 4-tuple, and the proof should not rest on
    // which rounding this machine happens to do (the m19.1a lesson, ADR-0060's addendum).
    for (std::uint32_t j = 0; j < hf->weight_rows; ++j) {
        for (std::uint32_t i = 0; i < hf->weight_columns; ++i) {
            unsigned sum = 0;
            for (std::uint32_t layer = 0; layer < HeightfieldAsset::kLayerCount; ++layer) {
                sum += hf->weights[hf->weight_index(i, j) + layer];
            }
            CHECK(sum == 255u);
        }
    }
}

TEST_CASE("every way a splat block can lie is refused, and counted") {
    SUBCASE("weights that do not sum to 255") {
        // The failure this check exists for: an unnormalized map dims or blows out the ground by a
        // few percent, which reads as a lighting bug rather than as a bad asset.
        HfSpec spec = splat_spec();
        spec.splat->weights[0] = 254; // texel (0,0) now sums to 254
        expect_rejected(wrap(payload_of(spec)), AssetError::InvalidHeightfield);
    }
    SUBCASE("a weight on an unused palette slot") {
        HfSpec spec = splat_spec();
        spec.splat->weights[2] = 10; // layer 2 is AssetId{0}
        spec.splat->weights[0] = 245;
        expect_rejected(wrap(payload_of(spec)), AssetError::InvalidHeightfield);
    }
    SUBCASE("a layer count this build cannot shade") {
        HfSpec spec = splat_spec();
        spec.splat->layer_count = 3;
        expect_rejected(wrap(payload_of(spec)), AssetError::InvalidHeightfield);
    }
    SUBCASE("an empty palette") {
        HfSpec spec = splat_spec();
        spec.splat->layers[0] = 0;
        spec.splat->weights = {0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0};
        expect_rejected(wrap(payload_of(spec)), AssetError::InvalidHeightfield);
    }
    SUBCASE("a weight blob that disagrees with its own dimensions") {
        HfSpec spec = splat_spec();
        spec.splat->weight_rows = 3; // 12 texels claimed, 4 texels' bytes present
        expect_rejected(wrap(payload_of(spec)), AssetError::SizeMismatch);
    }
    SUBCASE("a zero-sized weight grid") {
        HfSpec spec = splat_spec();
        spec.splat->weight_columns = 0;
        expect_rejected(wrap(payload_of(spec)), AssetError::InvalidHeightfield);
    }
    SUBCASE("a v2 header with no v2 body") {
        HfSpec spec;
        spec.version = 2; // and no splat block at all
        expect_rejected(wrap(payload_of(spec)), AssetError::Truncated);
    }
    SUBCASE("a payload version from the future") {
        HfSpec spec = splat_spec();
        spec.version = kHeightfieldPayloadVersion + 1;
        expect_rejected(wrap(payload_of(spec)), AssetError::UnsupportedVersion);
    }
}
