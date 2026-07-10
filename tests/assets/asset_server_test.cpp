// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the AssetServer (M6.5): asynchronous, non-blocking asset loading on the job system.
// Four properties are exercised, none with a sleep or a golden image:
//
//   1. PLACEHOLDER-UNTIL-PUMP. `request_*` returns immediately with a Loading handle; the handle
//      resolves to a visible placeholder (unit cube / magenta checker) and stays Loading — even
//      after the load job has finished — until the main thread calls pump(). This is the contract
//      that lets the render path never branch on "is it ready?".
//   2. PLACEHOLDER CONTENT. The placeholders really are the documented cube and 2x2 magenta
//   checker,
//      and an invalid handle resolves to them too.
//   3. DEDUP UNDER CONTENTION (the ThreadSanitizer proof). 64 requests issued from worker jobs over
//      8 distinct paths coalesce to exactly 8 physical loads, every handle reaches Ready, and the
//      asynchronously-decoded bytes equal a synchronous read of the same file — so no load ever
//      wrote into the wrong slot. This is the milestone's threading brick; the CI TSan job runs it.
//   4. FAILURE. A missing or corrupt file flips its handle to Failed, the placeholder persists, and
//      one clear error line is logged (via RIME_ERROR in the load job) — one bad asset never
//      poisons the others.
//
// The GPU-side proof (a real placeholder magenta pixel on screen swapped for the loaded texture)
// belongs to M6.6, where the render layer gains the upload-on-the-frame-thread drain: engine/assets
// depends only on core + platform, so a "ready" asset here is CPU-resident, not yet on the device.

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/platform/filesystem.hpp"

namespace fs = std::filesystem;
using namespace rime::assets;
using rime::core::JobSystem;

namespace {

const fs::path kFixtures{RIME_ASSETS_FIXTURE_DIR};

// A self-cleaning scratch directory so the concurrency test can mint many distinct paths (the unit
// of coalescing) without depending on filesystem state that outlives the run.
struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Decode a cooked file the blocking way — the yardstick the async path must reproduce
// byte-for-byte.
MeshAsset sync_read_mesh(const fs::path& p) {
    const auto bytes = rime::platform::read_file(p);
    REQUIRE(bytes);
    AssetError err{};
    auto mesh = read_mesh(*bytes, err);
    REQUIRE_MESSAGE(mesh, "sync read_mesh failed: ", to_string(err));
    return *mesh;
}

} // namespace

TEST_CASE("a requested asset is a placeholder until the main thread pumps it") {
    JobSystem jobs(2);
    AssetServer server(jobs);
    const fs::path mesh_path = kFixtures / "quad.rmesh";

    const MeshAssetHandle h = server.request_mesh(mesh_path);
    REQUIRE(h.is_valid());

    // Immediately after the request the load is in flight: Loading, no asset, placeholder returned.
    CHECK(server.state(h) == AssetState::Loading);
    CHECK(server.get(h) == nullptr);
    CHECK(&server.get_or_placeholder(h) == &server.placeholder_mesh());

    // The load JOB can finish, but the slot only leaves Loading in pump() (main thread) — this is
    // the guarantee that a getter and a completing job never race over the same slot's payload.
    server.wait_for_pending_loads();
    CHECK(server.state(h) == AssetState::Loading);

    const std::size_t promoted = server.pump();
    CHECK(promoted == 1);
    CHECK(server.state(h) == AssetState::Ready);
    REQUIRE(server.get(h) != nullptr);
    CHECK(&server.get_or_placeholder(h) != &server.placeholder_mesh()); // now the real asset

    // The asynchronously-decoded mesh is byte-identical to a synchronous read of the same file.
    const MeshAsset expected = sync_read_mesh(mesh_path);
    CHECK(server.get(h)->vertex_count == expected.vertex_count);
    CHECK(server.get(h)->vertices == expected.vertices);
    CHECK(server.get(h)->indices == expected.indices);
}

TEST_CASE("placeholders are the documented unit cube and 2x2 magenta checker") {
    JobSystem jobs(1);
    AssetServer server(jobs);

    const MeshAsset& cube = server.placeholder_mesh();
    CHECK(cube.vertex_count == 24);   // 6 faces x 4 corners
    CHECK(cube.indices.size() == 36); // 6 faces x 2 triangles x 3 indices
    CHECK(cube.vertex_stride == 32);  // position + normal + uv (the P|N|U v1 layout)

    const TextureAsset& checker = server.placeholder_texture();
    CHECK(checker.width == 2);
    CHECK(checker.height == 2);
    CHECK(checker.format == TextureFormat::Rgba8Srgb);
    CHECK(checker.mips.size() == 2);                       // 2x2 has a full chain of {2x2, 1x1}
    CHECK(checker.pixels.size() == 20);                    // 4 base texels + 1 mip texel, RGBA8
    CHECK(std::to_integer<int>(checker.pixels[0]) == 255); // (0,0) is magenta 255,0,255,255
    CHECK(std::to_integer<int>(checker.pixels[1]) == 0);
    CHECK(std::to_integer<int>(checker.pixels[2]) == 255);

    // An invalid handle resolves to the placeholder too, so extraction never dereferences null.
    CHECK(&server.get_or_placeholder(MeshAssetHandle{}) == &server.placeholder_mesh());
    CHECK(&server.get_or_placeholder(TextureAssetHandle{}) == &server.placeholder_texture());
}

TEST_CASE("concurrent requests coalesce per path — one physical load each") {
    // Mint K distinct paths per kind by copying the fixtures under fresh names; the server
    // coalesces on PATH, so N requests spread over K paths must trigger exactly K physical loads
    // regardless of how many worker threads raced into the per-path map at once.
    TempDir tmp("rime_m6_5_asset_server_dedup");
    const auto mesh_bytes = rime::platform::read_file(kFixtures / "quad.rmesh");
    const auto tex_bytes = rime::platform::read_file(kFixtures / "checker.rtex");
    REQUIRE(mesh_bytes);
    REQUIRE(tex_bytes);

    constexpr int kMeshPaths = 4;
    constexpr int kTexPaths = 4;
    std::vector<fs::path> mesh_paths;
    std::vector<fs::path> tex_paths;
    for (int k = 0; k < kMeshPaths; ++k) {
        const fs::path p = tmp.path / ("m" + std::to_string(k) + ".rmesh");
        REQUIRE(rime::platform::write_file(p, *mesh_bytes));
        mesh_paths.push_back(p);
    }
    for (int k = 0; k < kTexPaths; ++k) {
        const fs::path p = tmp.path / ("t" + std::to_string(k) + ".rtex");
        REQUIRE(rime::platform::write_file(p, *tex_bytes));
        tex_paths.push_back(p);
    }

    JobSystem jobs(4);
    AssetServer server(jobs);

    // 64 requests fanned out as jobs (request_* is legal from within a running job — the job-system
    // submit rule). Each job stores its handle at its own index, so the reads after wait() see
    // every write with no race. Even indices request meshes, odd request textures; the path cycles,
    // so each path is requested 8 times → heavy coalescing contention on 8 slots.
    constexpr int kN = 64;
    std::vector<MeshAssetHandle> mh(kN);
    std::vector<TextureAssetHandle> th(kN);
    JobSystem::Counter spawned{0};
    for (int i = 0; i < kN; ++i) {
        jobs.run(
            [&, i] {
                if (i % 2 == 0) {
                    mh[i] = server.request_mesh(mesh_paths[(i / 2) % kMeshPaths]);
                } else {
                    th[i] = server.request_texture(tex_paths[(i / 2) % kTexPaths]);
                }
            },
            &spawned);
    }
    jobs.wait(spawned);              // every request has been issued
    server.wait_for_pending_loads(); // every load job has finished
    server.pump();                   // promote all completed loads to Ready

    // The whole point: N=64 requests over 8 distinct paths = exactly 8 files read+decoded.
    CHECK(server.physical_load_count() == kMeshPaths + kTexPaths);

    for (int i = 0; i < kN; ++i) {
        if (i % 2 == 0) {
            REQUIRE(mh[i].is_valid());
            CHECK(server.state(mh[i]) == AssetState::Ready);
        } else {
            REQUIRE(th[i].is_valid());
            CHECK(server.state(th[i]) == AssetState::Ready);
        }
    }

    // Two requests for the same path coalesced onto one handle (index 0 and index 8 both hit mesh
    // path 0), and the resident bytes match a synchronous decode — proof the load hit the right
    // slot.
    CHECK(mh[0] == mh[8]);
    const MeshAsset expected = sync_read_mesh(mesh_paths[0]);
    REQUIRE(server.get(mh[0]) != nullptr);
    CHECK(server.get(mh[0])->vertices == expected.vertices);
}

TEST_CASE("a missing or corrupt file fails cleanly and keeps the placeholder") {
    TempDir tmp("rime_m6_5_asset_server_fail");
    // A file that exists but is not a valid RMA1 mesh (wrong magic) — the reader must reject it.
    const fs::path corrupt = tmp.path / "corrupt.rmesh";
    const std::vector<std::byte> junk(64, std::byte{0xAB});
    REQUIRE(rime::platform::write_file(corrupt, junk));

    JobSystem jobs(2);
    AssetServer server(jobs);

    const MeshAssetHandle missing = server.request_mesh(tmp.path / "does_not_exist.rmesh");
    const MeshAssetHandle bad = server.request_mesh(corrupt);
    const MeshAssetHandle good = server.request_mesh(kFixtures / "quad.rmesh");

    server.wait_for_pending_loads();
    server.pump();

    // Both failures land on Failed; the healthy request alongside them is unaffected.
    CHECK(server.state(missing) == AssetState::Failed);
    CHECK(server.state(bad) == AssetState::Failed);
    CHECK(server.state(good) == AssetState::Ready);

    // A failed handle still hands back the placeholder — the screen shows the missing-asset
    // stand-in, never a crash or a null.
    CHECK(server.get(missing) == nullptr);
    CHECK(&server.get_or_placeholder(missing) == &server.placeholder_mesh());
    CHECK(&server.get_or_placeholder(bad) == &server.placeholder_mesh());
    CHECK(server.get(good) != nullptr);
}
