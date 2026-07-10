// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rime/assets/mesh_asset.hpp"
#include "rime/assets/texture_asset.hpp"
#include "rime/core/jobs/job_system.hpp"

// Asynchronous asset loading (M6.5). The M6.1 registry loads synchronously — read, validate, keep —
// which stalls the frame. The AssetServer makes loading *structural async*: `request_*` returns a
// handle immediately (state = Loading) and fans the IO + parse + validate out as a job on the work-
// stealing system (core::JobSystem); a not-yet-ready handle resolves to a visible **placeholder**
// (magenta checker / unit cube) so the ECS render path never blocks on a pending asset.
//
// Two contracts make this safe and simple (measure before cleverness — ADR/house rule):
//   • THREADING. `request_*` may be called from the main thread OR from within a running job (the
//     JobSystem's own submit rule). Every piece of shared state lives behind one mutex; the heavy
//     work (file read + decode) runs OUTSIDE the lock, fully parallel. `pump()` and the getters are
//     main-thread. The destructor join-drains in-flight jobs before any member is torn down.
//   • GPU. `engine/assets` depends only on core + platform (never the RHI), so a "ready" asset is
//     CPU-resident and validated, not yet uploaded. The GPU upload, on the frame thread, is the
//     render layer's job at M6.6, draining the set that `pump()` newly readies. Keeping the seam
//     here, not at the upload, is what lets the asset layer stay device-agnostic.
//
// Out of scope (documented seams): priorities, memory budgets, eviction, hot reload, decompression.
namespace rime::assets {

// The lifecycle of one requested asset. Ready means CPU-resident + validated (see the GPU note
// above). Queried lock-cheap; a handle only ever moves Loading → Ready | Failed.
enum class AssetState : std::uint8_t { Loading, Ready, Failed };

// A handle to a requested asset, phantom-typed on the asset kind so a mesh handle can't be passed
// where a texture handle is wanted. It is just a dense index into the server's slot storage (there
// is no eviction in v1, so an index never dangles); hold the handle, not the pointer a getter
// returns.
template <class T> struct AssetHandle {
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
    std::uint32_t index = kInvalid;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != kInvalid; }

    friend constexpr bool operator==(AssetHandle a, AssetHandle b) noexcept {
        return a.index == b.index;
    }
};

using MeshAssetHandle = AssetHandle<MeshAsset>;
using TextureAssetHandle = AssetHandle<TextureAsset>;

class AssetServer {
public:
    // Borrows the job system (owned by the app). Builds the placeholders up front so a getter can
    // always return something valid, even before the first pump().
    explicit AssetServer(core::JobSystem& jobs);
    ~AssetServer();

    AssetServer(const AssetServer&) = delete;
    AssetServer& operator=(const AssetServer&) = delete;

    // Request a cooked mesh/texture. Returns immediately with a Loading handle and fans the load
    // out as a job. A repeat request for the SAME path coalesces onto the first handle (one
    // physical load per path — the de-dup the proof counts). Callable from the main thread or from
    // within a job.
    [[nodiscard]] MeshAssetHandle request_mesh(const std::filesystem::path& path);
    [[nodiscard]] TextureAssetHandle request_texture(const std::filesystem::path& path);

    [[nodiscard]] AssetState state(MeshAssetHandle handle) const;
    [[nodiscard]] AssetState state(TextureAssetHandle handle) const;

    // The loaded asset, or nullptr if the handle is invalid / not yet Ready. The `_or_placeholder`
    // form never returns null — it is what the render extraction calls, so recording never
    // branches.
    [[nodiscard]] const MeshAsset* get(MeshAssetHandle handle) const;
    [[nodiscard]] const TextureAsset* get(TextureAssetHandle handle) const;
    [[nodiscard]] const MeshAsset& get_or_placeholder(MeshAssetHandle handle) const;
    [[nodiscard]] const TextureAsset& get_or_placeholder(TextureAssetHandle handle) const;

    // Main thread, once per frame: move every completed CPU load into resident storage and flip its
    // handle to Ready (or Failed). Returns how many handles newly resolved. This is the frame point
    // the render layer's GPU-upload drain will hang off (M6.6).
    std::size_t pump();

    // Block until every load submitted *so far* has finished, participating in the job system while
    // it waits (so it never deadlocks even if all workers are busy). Main thread. This is the
    // synchronous escape hatch: a loading screen — or a test asserting the resident set — does
    // `request_*(); … ; wait_for_pending_loads(); pump();` to force "load these now". It does NOT
    // stop new requests; it drains what was already in flight. The destructor calls it first so no
    // job outlives the queues it writes into.
    void wait_for_pending_loads();

    // How many files the server actually read+decoded — the de-dup counter the proof asserts
    // against (N requests for K distinct paths ⇒ K physical loads).
    [[nodiscard]] std::size_t physical_load_count() const noexcept {
        return physical_loads_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const MeshAsset& placeholder_mesh() const noexcept { return placeholder_mesh_; }

    [[nodiscard]] const TextureAsset& placeholder_texture() const noexcept {
        return placeholder_texture_;
    }

private:
    template <class T> struct Slot {
        AssetState state = AssetState::Loading;
        std::optional<T> asset; // populated by pump() on the main thread when the load completes
    };

    void load_mesh_job(std::uint32_t index, std::filesystem::path path);
    void load_texture_job(std::uint32_t index, std::filesystem::path path);

    core::JobSystem& jobs_;

    // One lock over all bookkeeping; the decode work happens outside it. std::deque so a getter's
    // pointer into a slot stays valid across later requests (unlike a reallocating vector).
    mutable std::mutex mu_;
    std::deque<Slot<MeshAsset>> mesh_slots_;
    std::deque<Slot<TextureAsset>> tex_slots_;
    std::unordered_map<std::string, std::uint32_t> mesh_by_path_; // path → slot, for coalescing
    std::unordered_map<std::string, std::uint32_t> tex_by_path_;
    std::vector<std::pair<std::uint32_t, MeshAsset>>
        mesh_done_; // completed CPU loads awaiting pump()
    std::vector<std::pair<std::uint32_t, TextureAsset>> tex_done_;

    std::atomic<std::size_t> physical_loads_{0};
    core::JobSystem::Counter inflight_{0}; // in-flight job count; ~AssetServer waits on it

    MeshAsset placeholder_mesh_;
    TextureAsset placeholder_texture_;
};

} // namespace rime::assets
