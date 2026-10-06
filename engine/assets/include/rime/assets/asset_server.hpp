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

#include "rime/assets/heightfield_asset.hpp"
#include "rime/assets/material_asset.hpp"
#include "rime/assets/mesh_asset.hpp"
#include "rime/assets/terrain_layer_asset.hpp"
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
// STREAMED KINDS (m19.8b, ADR-0067). Meshes, textures and materials are RETAINED: once requested
// they stay resident for the server's lifetime, which is right for a level's fixed content and is
// why their handle is a bare index. Terrain is different — a world of tiles cannot all be resident
// — so heightfields and terrain layers are requested through a second, RELEASABLE path:
//   • every `request_heightfield` / `request_terrain_layer` call takes one OWNERSHIP of the slot
//   and
//     must be paired with exactly one `release`;
//   • when the last owner releases, the slot is EVICTED: its CPU payload is freed and its index
//     goes on a free list, to be reused with a bumped GENERATION;
//   • a handle carries the generation it was issued under, so a handle that outlived its slot
//     resolves to "not available" (AssetState::Stale / nullptr) — never to the index's new tenant;
//   • a release that lands while the load job is still in flight only DEFERS the eviction: the job
//     itself evicts when it finishes, so nothing is ever freed (or its index reused) under a
//     running job.
// Every one of those paths has a counter (StreamCounters) — CLAUDE.md guardrail 5.
//
// m19.8e (ADR-0073) adds a third streamed kind: TEXTURES requested through
// `request_streamed_texture`. Terrain appearance bakes go this way so their CPU copy is freed the
// moment the GPU has its own — the retained texture path would keep every bake ever loaded.
//
// Out of scope (documented seams): priorities, memory budgets, hot reload, decompression. Eviction
// POLICY (what to release, and when) is the caller's — the server only honours releases.
namespace rime::assets {

// The lifecycle of one requested asset. Ready means CPU-resident + validated (see the GPU note
// above). Queried lock-cheap; a retained handle only ever moves Loading → Ready | Failed. Stale is
// reported only for the streamed kinds: the handle's slot was evicted (or the handle's ownership
// was already released), so there is nothing behind it any more — distinct from Failed, which
// means "the load ran and the file was bad" and is worth surfacing to a human.
enum class AssetState : std::uint8_t { Loading, Ready, Failed, Stale };

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
using MaterialAssetHandle = AssetHandle<MaterialAsset>;

// A handle to a STREAMED asset: the slot index plus the generation the slot had when this handle
// was issued. The index alone would be a lie the moment eviction exists — after a release the same
// index may hold a different tile — so every lookup compares generations first (the classic
// generational-index scheme; the ECS entity id does the same for the same reason).
//
// WHY A SEPARATE TYPE, AND WHY EXPLICIT release() RATHER THAN AN RAII OWNER. The retained
// AssetHandle is a trivially-copyable 4-byte value that lives in ECS components and is compared
// and copied freely; giving it a generation or a destructor would change every existing user for
// kinds that are never evicted. And an RAII owner would have to carry a back-pointer to the
// server, be move-only, and could no longer sit in plain component data or cross a job boundary
// by value — the opposite of how handles are used here. So the handle stays a plain value and
// ownership is an explicit count on the slot; the residency layer above (m19.8a's tile table),
// which already tracks one record per tile, is the natural place to pair request with release.
// The cost of that choice is stated plainly: copies of a handle share ONE ownership, and
// releasing twice through two copies is a caller bug the server can only partly detect (see
// release()).
template <class T> struct StreamedAssetHandle {
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
    std::uint32_t index = kInvalid;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != kInvalid; }

    friend constexpr bool operator==(StreamedAssetHandle a, StreamedAssetHandle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
};

using HeightfieldAssetHandle = StreamedAssetHandle<HeightfieldAsset>;
using TerrainLayerAssetHandle = StreamedAssetHandle<TerrainLayerAsset>;
// m19.8e (ADR-0073): a texture on the RELEASABLE path — a terrain parent's appearance bake, whose
// CPU copy is useless once it is on the GPU. A different type from the retained
// `TextureAssetHandle`, so the two can never be confused: the same file requested both ways is two
// slots in two pools, one retained and one released.
using StreamedTextureAssetHandle = StreamedAssetHandle<TextureAsset>;

// What the streamed path did, by outcome (guardrail 5: every skip / drop / defer path is counted,
// because a proof that cannot see what was skipped still reads as passing). These cover the
// STREAMED kinds only — the retained mesh/texture/material path is unchanged and uncounted here.
// At quiescence (no load in flight) they obey two conservation laws the tests assert:
//     requests            == coalesced_requests + loads_started
//     loads_started       == evictions + (slots still owned)
//     deferred_evictions  == cancelled_evictions + (evictions performed by a finishing load job)
struct StreamCounters {
    std::uint64_t requests = 0; // every request_heightfield / _terrain_layer / _streamed_texture
    std::uint64_t coalesced_requests = 0; // …that joined an existing slot instead of loading
    std::uint64_t loads_started = 0;      // …that claimed a slot and submitted a load job
    std::uint64_t failed_loads = 0;       // load jobs that could not read or validate their file
    std::uint64_t evictions = 0;          // slots freed (payload destroyed, index recyclable)
    // A last-owner release that found the load still in flight: the eviction is owed, not done.
    std::uint64_t deferred_evictions = 0;
    // An owed eviction that never happened because a new request for the same path arrived first
    // and adopted the in-flight load (so the file is read once, not twice).
    std::uint64_t cancelled_evictions = 0;
    // state() / get() / release() calls whose handle no longer names a live ownership: the slot
    // was evicted (generation mismatch), or it has no owner left. An invalid (default) handle is
    // not counted — that is "no handle", not a stale one.
    std::uint64_t stale_handle_resolutions = 0;
};

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
    // Materials (m16.3) complete the set. A cooked material is a fixed 100-byte record, so the load
    // is trivially cheap — but it goes through the same async slot machinery as the other two
    // rather than being read synchronously, because a material is the FIRST level of a two-level
    // dependency: its five texture ids are only known once it is Ready, and the textures cannot be
    // requested before then. Making it async here is what lets one settle loop drive the whole
    // chain instead of the bridge special-casing materials.
    [[nodiscard]] MaterialAssetHandle request_material(const std::filesystem::path& path);

    // Request a cooked heightfield / terrain layer (m19.8b). Same shape as the calls above —
    // returns at once with a Loading handle, one job per path, repeat requests coalesce — with one
    // addition: EVERY call takes an ownership of the slot, the coalesced ones included, and each
    // must be balanced by one release(). Callable from the main thread or from within a job.
    //
    // There is no `get_or_placeholder` for these on purpose. A placeholder texture is a visible
    // "missing" tell; a placeholder heightfield would be flat ground that things could stand on,
    // collide with and replicate. Unavailable terrain must stay visibly unavailable (nullptr) so
    // the caller defers, rather than simulating against ground that is not there.
    [[nodiscard]] HeightfieldAssetHandle request_heightfield(const std::filesystem::path& path);
    [[nodiscard]] TerrainLayerAssetHandle request_terrain_layer(const std::filesystem::path& path);
    // m19.8e: a texture on the streamed path (see StreamedTextureAssetHandle). Same contract.
    [[nodiscard]] StreamedTextureAssetHandle
    request_streamed_texture(const std::filesystem::path& path);

    // Give up one ownership. Returns true if the handle named a live ownership. When the last one
    // goes: if the load is not in flight the slot is evicted NOW (the payload is freed before this
    // returns, and any pointer a getter handed out for it is dead); if the load job is still in
    // flight the eviction is deferred to the moment that job finishes. A stale handle — evicted
    // slot, or a slot with no owner left — is refused, counted, and changes nothing. Callable from
    // the main thread or from within a job.
    //
    // What the server CANNOT detect: two copies of one handle each released while a second,
    // independent owner still holds the slot. The count cannot tell whose ownership a release
    // spends. One request, one release.
    bool release(HeightfieldAssetHandle handle);
    bool release(TerrainLayerAssetHandle handle);
    bool release(StreamedTextureAssetHandle handle);

    [[nodiscard]] AssetState state(MeshAssetHandle handle) const;
    [[nodiscard]] AssetState state(TextureAssetHandle handle) const;
    [[nodiscard]] AssetState state(MaterialAssetHandle handle) const;
    // Loading / Ready / Failed for a live handle, Stale for one whose slot is gone; an invalid
    // handle is Failed, as for the retained kinds.
    [[nodiscard]] AssetState state(HeightfieldAssetHandle handle) const;
    [[nodiscard]] AssetState state(TerrainLayerAssetHandle handle) const;
    [[nodiscard]] AssetState state(StreamedTextureAssetHandle handle) const;

    // The loaded asset, or nullptr if the handle is invalid / not yet Ready. The `_or_placeholder`
    // form never returns null — it is what the render extraction calls, so recording never
    // branches.
    [[nodiscard]] const MeshAsset* get(MeshAssetHandle handle) const;
    [[nodiscard]] const TextureAsset* get(TextureAssetHandle handle) const;
    [[nodiscard]] const MaterialAsset* get(MaterialAssetHandle handle) const;
    // Streamed getters: nullptr unless the handle is live AND Ready. The pointer is valid only
    // while the caller's own ownership is held — after the last release() it dangles, so re-get
    // through the handle each frame rather than caching the pointer across a release point.
    [[nodiscard]] const HeightfieldAsset* get(HeightfieldAssetHandle handle) const;
    [[nodiscard]] const TerrainLayerAsset* get(TerrainLayerAssetHandle handle) const;
    [[nodiscard]] const TextureAsset* get(StreamedTextureAssetHandle handle) const;
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

    // A consistent snapshot of the streamed-path counters (taken under the lock).
    [[nodiscard]] StreamCounters stream_counters() const;

    // How many streamed slots are occupied right now (owned, or in flight with an eviction owed),
    // and how many of those hold a decoded payload. An eviction lowers both; these are what "the
    // memory was actually freed" is asserted against.
    [[nodiscard]] std::size_t live_heightfield_slots() const;
    [[nodiscard]] std::size_t resident_heightfields() const;
    [[nodiscard]] std::size_t live_terrain_layer_slots() const;
    [[nodiscard]] std::size_t resident_terrain_layers() const;
    [[nodiscard]] std::size_t live_streamed_texture_slots() const;
    [[nodiscard]] std::size_t resident_streamed_textures() const;

    // m19.8e: the decoded bytes the streamed kinds hold right now (payloads parked in a slot, Ready
    // or awaiting pump()). What "the CPU copy was released" is measured in: an eviction lowers it
    // by exactly what the load raised it by. Heightfields count their samples and weights; textures
    // their pixels; a terrain layer is a fixed record and counts its size.
    [[nodiscard]] std::uint64_t resident_streamed_bytes() const;
    [[nodiscard]] std::uint64_t resident_streamed_texture_bytes() const;

    [[nodiscard]] const MeshAsset& placeholder_mesh() const noexcept { return placeholder_mesh_; }

    [[nodiscard]] const TextureAsset& placeholder_texture() const noexcept {
        return placeholder_texture_;
    }

private:
    template <class T> struct Slot {
        AssetState state = AssetState::Loading;
        std::optional<T> asset; // populated by pump() on the main thread when the load completes
    };

    // One streamed slot. `generation` survives eviction (it is what makes old handles stale);
    // everything else is reset. All fields are guarded by mu_.
    template <class T> struct StreamSlot {
        AssetState state = AssetState::Loading;
        std::uint32_t generation = 0;
        std::uint32_t owners = 0; // outstanding request_* calls not yet released
        bool occupied = false;    // false ⇒ on the free list; no handle may resolve to it
        // The load job has been submitted and has not yet published. While this is set the slot
        // is PINNED: it cannot be evicted and its index cannot be reused, whatever `owners` says,
        // because the job will write into this slot by index when it finishes.
        bool in_flight = false;
        // The decoded payload. Written by the load job (under the lock) while the state is still
        // Loading — invisible to getters until pump() flips the state to Ready — and destroyed by
        // eviction.
        std::optional<T> asset;
        std::string key; // the path this slot serves, to un-map it on eviction
    };

    // The per-kind storage of the streamed path. std::deque for the same reason as above: a
    // getter's pointer must survive later requests growing the container.
    template <class T> struct StreamPool {
        std::deque<StreamSlot<T>> slots;
        std::vector<std::uint32_t> free;                        // evicted indices, reusable
        std::unordered_map<std::string, std::uint32_t> by_path; // path → occupied slot
        std::vector<std::uint32_t> done;                        // loaded, awaiting pump()
        std::size_t live = 0;                                   // occupied slots
        std::size_t resident = 0;                               // occupied slots with a payload
        std::uint64_t bytes = 0; // m19.8e: Σ payload_bytes over the payloads held
    };

    template <class T> StreamPool<T>& stream_pool() noexcept;
    template <class T> const StreamPool<T>& stream_pool() const noexcept;

    template <class T>
    [[nodiscard]] StreamedAssetHandle<T> request_streamed(const std::filesystem::path& path);
    template <class T> void load_streamed_job(std::uint32_t index, std::filesystem::path path);
    template <class T> bool release_streamed(StreamedAssetHandle<T> handle);
    template <class T> [[nodiscard]] AssetState state_streamed(StreamedAssetHandle<T> handle) const;
    template <class T> [[nodiscard]] const T* get_streamed(StreamedAssetHandle<T> handle) const;
    // The slot a handle names if it is a LIVE ownership, else nullptr (counting a stale one).
    // Caller holds mu_.
    template <class T>
    [[nodiscard]] const StreamSlot<T>* resolve_locked(StreamedAssetHandle<T> handle) const;
    // Free a slot: un-map its path, bump its generation, recycle its index. The payload is moved
    // out into the return value so the caller can destroy it AFTER dropping mu_ (a heightfield's
    // sample vector can be megabytes; freeing it need not stall every other request). Caller
    // holds mu_.
    template <class T>
    [[nodiscard]] std::optional<T> evict_locked(StreamPool<T>& pool, std::uint32_t index);

    void load_mesh_job(std::uint32_t index, std::filesystem::path path);
    void load_texture_job(std::uint32_t index, std::filesystem::path path);
    void load_material_job(std::uint32_t index, std::filesystem::path path);

    core::JobSystem& jobs_;

    // One lock over all bookkeeping — slots, path maps, completion queues, the streamed pools and
    // their counters; the decode work happens outside it. std::deque so a getter's
    // pointer into a slot stays valid across later requests (unlike a reallocating vector).
    mutable std::mutex mu_;
    std::deque<Slot<MeshAsset>> mesh_slots_;
    std::deque<Slot<TextureAsset>> tex_slots_;
    std::deque<Slot<MaterialAsset>> mat_slots_;
    std::unordered_map<std::string, std::uint32_t> mesh_by_path_; // path → slot, for coalescing
    std::unordered_map<std::string, std::uint32_t> tex_by_path_;
    std::unordered_map<std::string, std::uint32_t> mat_by_path_;
    std::vector<std::pair<std::uint32_t, MeshAsset>>
        mesh_done_; // completed CPU loads awaiting pump()
    std::vector<std::pair<std::uint32_t, TextureAsset>> tex_done_;
    std::vector<std::pair<std::uint32_t, MaterialAsset>> mat_done_;

    // The streamed kinds (m19.8b). Same lock: mu_ guards both pools and the counters, so a
    // request, a release and a finishing job are totally ordered against each other — which is
    // the whole correctness argument for deferred eviction (see load_streamed_job).
    StreamPool<HeightfieldAsset> hf_pool_;
    StreamPool<TerrainLayerAsset> layer_pool_;
    StreamPool<TextureAsset> stex_pool_; // m19.8e: streamed textures (terrain bakes)
    // mutable: a const getter that meets a stale handle counts it (the read is what found it).
    mutable StreamCounters stream_counters_;

    std::atomic<std::size_t> physical_loads_{0};
    core::JobSystem::Counter inflight_{0}; // in-flight job count; ~AssetServer waits on it

    MeshAsset placeholder_mesh_;
    TextureAsset placeholder_texture_;
};

} // namespace rime::assets
