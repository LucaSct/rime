// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Cooked-file encoders shared by the terrain residency proofs (m19.8a) and the LOD proofs
// (m19.8d2): the inverse of cooked_reader.cpp's decoders, so a test can write a real world to disk
// and drive it through the AssetServer's streamed handles. m19.8a's encoder round-trip case checks
// them against the engine's own readers before anything relies on them.
#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/heightfield_asset.hpp"

namespace rime::terrain_test {

namespace fs = std::filesystem;
using assets::AssetId;

struct Writer {
    std::vector<std::byte> b;

    void u8(std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); }

    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFF));
        u8(static_cast<std::uint8_t>(v >> 8));
    }

    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }

    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }

    void f32(float f) {
        std::uint32_t u = 0;
        std::memcpy(&u, &f, sizeof(u));
        u32(u);
    }
};

inline std::vector<std::byte>
rma1(assets::AssetKind kind, std::uint64_t schema, const std::vector<std::byte>& payload) {
    Writer w;
    for (const std::byte c : assets::kCookedMagic) {
        w.b.push_back(c);
    }
    w.u16(assets::kContainerVersion);
    w.u16(static_cast<std::uint16_t>(kind));
    w.u64(schema);
    w.u64(payload.size());
    w.b.insert(w.b.end(), payload.begin(), payload.end());
    return w.b;
}

inline std::vector<std::byte> encode_heightfield(const assets::HeightfieldAsset& a) {
    Writer w;
    const bool splat = a.has_splat();
    w.u32(splat ? 2u : 1u);
    w.u32(a.columns);
    w.u32(a.rows);
    w.f32(a.cell_size_x);
    w.f32(a.cell_size_z);
    w.f32(a.origin.x);
    w.f32(a.origin.y);
    w.f32(a.origin.z);
    w.f32(a.height_scale);
    w.f32(a.height_offset);
    w.u32(static_cast<std::uint32_t>(a.triangulation));
    const auto [lo, hi] = std::minmax_element(a.samples.begin(), a.samples.end());
    w.u32(*lo);
    w.u32(*hi);
    for (const std::uint16_t q : a.samples) {
        w.u16(q);
    }
    if (splat) {
        w.u32(a.weight_columns);
        w.u32(a.weight_rows);
        w.u32(assets::HeightfieldAsset::kLayerCount);
        for (const AssetId id : a.layers) {
            w.u64(id.value);
        }
        for (const std::uint8_t x : a.weights) {
            w.u8(x);
        }
    }
    return rma1(assets::AssetKind::Heightfield, assets::heightfield_schema_hash(), w.b);
}

inline void write_file(const fs::path& p, const std::vector<std::byte>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    REQUIRE(f.good());
}

struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& name) {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("rime-terrain-" + name + "-" + std::to_string(counter++) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

} // namespace rime::terrain_test
