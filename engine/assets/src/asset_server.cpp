// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/assets/asset_server.hpp"

#include <algorithm>
#include <span>
#include <type_traits>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/byte_cursor.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/platform/filesystem.hpp"

namespace rime::assets {

namespace {

// The "mesh not loaded yet" stand-in: a unit cube (±0.5), 24 vertices with per-face normals, 36
// indices. Generated per face from an outward normal + two in-plane axes so it stays compact and
// obviously correct — a closed, visible-from-any-angle shape, unlike a lone triangle.
MeshAsset make_placeholder_mesh() {
    constexpr float h = 0.5f;
    // {normal, uAxis, vAxis} per face; corners are ±1 combinations of the two in-plane axes.
    constexpr float kFaces[6][3][3] = {
        {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}},
    };
    constexpr int kCorners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

    MeshAsset mesh;
    mesh.attribs = kMeshV1Attribs;
    mesh.vertex_stride = 32;
    core::ByteWriter blob(mesh.vertices);
    std::uint32_t vertex_count = 0;
    for (const auto& f : kFaces) {
        const auto& n = f[0];
        const auto& u = f[1];
        const auto& v = f[2];
        const std::uint32_t base = vertex_count;
        for (const auto& c : kCorners) {
            for (int k = 0; k < 3; ++k) {
                blob.f32(
                    h * (n[k] + static_cast<float>(c[0]) * u[k] + static_cast<float>(c[1]) * v[k]));
            }
            blob.f32(n[0]);
            blob.f32(n[1]);
            blob.f32(n[2]);
            blob.f32(c[0] > 0 ? 1.0f : 0.0f);
            blob.f32(c[1] > 0 ? 1.0f : 0.0f);
            ++vertex_count;
        }
        for (const std::uint32_t i : {base, base + 1, base + 2, base, base + 2, base + 3}) {
            mesh.indices.push_back(i);
        }
    }
    mesh.vertex_count = vertex_count;
    mesh.bounds = {{-h, -h, -h}, {h, h, h}};
    mesh.submeshes.push_back({0, static_cast<std::uint32_t>(mesh.indices.size()), 0});
    return mesh;
}

// The "texture not loaded yet" stand-in: a 2×2 magenta/black checker + its 1×1 mip. Magenta is the
// universal "missing texture" tell, so a placeholder is instantly recognisable on screen.
TextureAsset make_placeholder_texture() {
    constexpr std::uint8_t kMagenta[4] = {255, 0, 255, 255};
    constexpr std::uint8_t kBlack[4] = {0, 0, 0, 255};
    TextureAsset tex;
    tex.width = 2;
    tex.height = 2;
    tex.format = TextureFormat::Rgba8Srgb;
    const auto push = [&](const std::uint8_t (&px)[4]) {
        for (const std::uint8_t b : px) {
            tex.pixels.push_back(static_cast<std::byte>(b));
        }
    };
    push(kMagenta); // (0,0)
    push(kBlack);   // (1,0)
    push(kBlack);   // (0,1)
    push(kMagenta); // (1,1)
    push(kMagenta); // the 1×1 mip
    tex.mips.push_back({2, 2, 0, 16});
    tex.mips.push_back({1, 1, 16, 4});
    return tex;
}

// What differs between the streamed kinds: a name for the log line and the one-call reader. The
// rest of the streamed path (slots, ownership, eviction) is written once, as templates over T.
template <class T> struct StreamKind;

template <> struct StreamKind<HeightfieldAsset> {
    static constexpr const char* kName = "heightfield";

    static std::optional<HeightfieldAsset> read(std::span<const std::byte> file, AssetError& err) {
        return read_heightfield(file, err);
    }

    // The decoded payload's bulk: the samples and the splat weights (m19.8e's CPU accounting).
    static std::uint64_t bytes(const HeightfieldAsset& h) {
        return h.samples.size() * sizeof(std::uint16_t) + h.weights.size();
    }
};

template <> struct StreamKind<TerrainLayerAsset> {
    static constexpr const char* kName = "terrain layer";

    static std::optional<TerrainLayerAsset> read(std::span<const std::byte> file, AssetError& err) {
        return read_terrain_layer(file, err);
    }

    static std::uint64_t bytes(const TerrainLayerAsset&) { return sizeof(TerrainLayerAsset); }
};

// m19.8e: the streamed texture kind — the same reader as the retained path.
template <> struct StreamKind<TextureAsset> {
    static constexpr const char* kName = "streamed texture";

    static std::optional<TextureAsset> read(std::span<const std::byte> file, AssetError& err) {
        return read_texture(file, err);
    }

    static std::uint64_t bytes(const TextureAsset& t) { return t.pixels.size(); }
};

// The coalescing key. Requests merge on the file they name, so two spellings of one file
// ("a/f.mesh", "./a/f.mesh", "a/b/../f.mesh", or "a\\f.mesh" on Windows) must produce ONE key;
// otherwise the file is read and decoded twice, two slots and GPU uploads are held for one asset,
// and physical_load_count() double-counts.
//  * lexically_normal() folds "./", "x/../" and duplicate separators purely as text -- it never
//    touches the filesystem, because request_* runs before the file is known to exist and must
//    not pay a stat.
//  * generic_string() spells every separator '/', so '\\' and '/' agree on Windows.
//  * Deliberately NO case-folding: Linux is case-sensitive, so lowercasing would merge two
//    genuinely different files. On Windows two spellings differing only in case still miss each
//    other, which costs a duplicate load but is never wrong.
std::string cache_key(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

} // namespace

AssetServer::AssetServer(core::JobSystem& jobs)
    : jobs_(jobs), placeholder_mesh_(make_placeholder_mesh()),
      placeholder_texture_(make_placeholder_texture()) {}

AssetServer::~AssetServer() {
    // Join-drain: run every in-flight load to completion before any member (the mutex, the queues
    // the jobs write) is destroyed. wait() participates on this (main) thread, so it never
    // deadlocks.
    wait_for_pending_loads();
}

void AssetServer::wait_for_pending_loads() {
    jobs_.wait(inflight_);
}

MeshAssetHandle AssetServer::request_mesh(const std::filesystem::path& path) {
    const std::string key = cache_key(path);
    std::uint32_t index;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (const auto it = mesh_by_path_.find(key); it != mesh_by_path_.end()) {
            return MeshAssetHandle{it->second}; // coalesce onto the in-flight/loaded request
        }
        index = static_cast<std::uint32_t>(mesh_slots_.size());
        mesh_slots_.emplace_back();
        mesh_by_path_.emplace(key, index);
    }
    jobs_.run([this, index, path] { load_mesh_job(index, path); }, &inflight_);
    return MeshAssetHandle{index};
}

TextureAssetHandle AssetServer::request_texture(const std::filesystem::path& path) {
    const std::string key = cache_key(path);
    std::uint32_t index;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (const auto it = tex_by_path_.find(key); it != tex_by_path_.end()) {
            return TextureAssetHandle{it->second};
        }
        index = static_cast<std::uint32_t>(tex_slots_.size());
        tex_slots_.emplace_back();
        tex_by_path_.emplace(key, index);
    }
    jobs_.run([this, index, path] { load_texture_job(index, path); }, &inflight_);
    return TextureAssetHandle{index};
}

MaterialAssetHandle AssetServer::request_material(const std::filesystem::path& path) {
    const std::string key = cache_key(path);
    std::uint32_t index;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (const auto it = mat_by_path_.find(key); it != mat_by_path_.end()) {
            return MaterialAssetHandle{it->second};
        }
        index = static_cast<std::uint32_t>(mat_slots_.size());
        mat_slots_.emplace_back();
        mat_by_path_.emplace(key, index);
    }
    jobs_.run([this, index, path] { load_material_job(index, path); }, &inflight_);
    return MaterialAssetHandle{index};
}

void AssetServer::load_mesh_job(std::uint32_t index, std::filesystem::path path) {
    physical_loads_.fetch_add(1, std::memory_order_relaxed);
    // IO + decode happen OUTSIDE the lock — this is the parallel part.
    std::optional<MeshAsset> mesh;
    if (const std::optional<std::vector<std::byte>> bytes = platform::read_file(path)) {
        AssetError error = AssetError::Io;
        mesh = read_mesh(*bytes, error);
        if (!mesh) {
            RIME_ERROR("assets: async mesh load '{}' failed: {}", path.string(), to_string(error));
        }
    } else {
        RIME_ERROR("assets: async mesh load cannot open '{}'", path.string());
    }
    // Publish under the lock: a completed load queues for pump(); a failure flips the slot now.
    std::lock_guard<std::mutex> lock(mu_);
    if (mesh) {
        mesh_done_.emplace_back(index, std::move(*mesh));
    } else {
        mesh_slots_[index].state = AssetState::Failed;
    }
}

void AssetServer::load_texture_job(std::uint32_t index, std::filesystem::path path) {
    physical_loads_.fetch_add(1, std::memory_order_relaxed);
    std::optional<TextureAsset> tex;
    if (const std::optional<std::vector<std::byte>> bytes = platform::read_file(path)) {
        AssetError error = AssetError::Io;
        tex = read_texture(*bytes, error);
        if (!tex) {
            RIME_ERROR(
                "assets: async texture load '{}' failed: {}", path.string(), to_string(error));
        }
    } else {
        RIME_ERROR("assets: async texture load cannot open '{}'", path.string());
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (tex) {
        tex_done_.emplace_back(index, std::move(*tex));
    } else {
        tex_slots_[index].state = AssetState::Failed;
    }
}

void AssetServer::load_material_job(std::uint32_t index, std::filesystem::path path) {
    physical_loads_.fetch_add(1, std::memory_order_relaxed);
    std::optional<MaterialAsset> mat;
    if (const std::optional<std::vector<std::byte>> bytes = platform::read_file(path)) {
        AssetError error = AssetError::Io;
        mat = read_material(*bytes, error);
        if (!mat) {
            RIME_ERROR(
                "assets: async material load '{}' failed: {}", path.string(), to_string(error));
        }
    } else {
        RIME_ERROR("assets: async material load cannot open '{}'", path.string());
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (mat) {
        mat_done_.emplace_back(index, std::move(*mat));
    } else {
        mat_slots_[index].state = AssetState::Failed;
    }
}

// ─── The streamed kinds (m19.8b, ADR-0067) ──────────────────────────────────────────────────────
//
// The technique is a GENERATIONAL SLOT MAP with an ownership count per slot. A slot is in exactly
// one of three conditions, and mu_ makes every transition between them atomic:
//
//   FREE      occupied == false. On the free list. Its generation is already one past every
//             handle ever issued for it, so no handle resolves to it.
//   OWNED     occupied, owners > 0. Resident (or loading, or Failed) and reachable by handle.
//   OWED      occupied, owners == 0, in_flight. The last owner left while the load job was still
//             running. The slot stays occupied — pinned by the job, which will write into it by
//             index — and the job evicts it on its way out. No handle resolves to it.
//
// There is deliberately no fourth "occupied, unowned, idle" condition: the two places an owner
// count can reach zero with no job in flight (release, and a job finishing an OWED slot) both
// evict before they drop the lock.

template <class T> AssetServer::StreamPool<T>& AssetServer::stream_pool() noexcept {
    if constexpr (std::is_same_v<T, HeightfieldAsset>) {
        return hf_pool_;
    } else if constexpr (std::is_same_v<T, TextureAsset>) {
        return stex_pool_;
    } else {
        static_assert(std::is_same_v<T, TerrainLayerAsset>);
        return layer_pool_;
    }
}

template <class T> const AssetServer::StreamPool<T>& AssetServer::stream_pool() const noexcept {
    if constexpr (std::is_same_v<T, HeightfieldAsset>) {
        return hf_pool_;
    } else if constexpr (std::is_same_v<T, TextureAsset>) {
        return stex_pool_;
    } else {
        static_assert(std::is_same_v<T, TerrainLayerAsset>);
        return layer_pool_;
    }
}

template <class T>
StreamedAssetHandle<T> AssetServer::request_streamed(const std::filesystem::path& path) {
    // Same normalisation as the retained path, and safe here by construction: the slot records
    // this key (slot.key = key below) and eviction erases by slot.key, so the map and the slot
    // always agree on the spelling.
    const std::string key = cache_key(path);
    StreamPool<T>& pool = stream_pool<T>();
    StreamedAssetHandle<T> handle;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++stream_counters_.requests;
        if (const auto it = pool.by_path.find(key); it != pool.by_path.end()) {
            // Coalesce — and take an ownership, which is the difference from the retained path.
            StreamSlot<T>& slot = pool.slots[it->second];
            ++stream_counters_.coalesced_requests;
            if (slot.owners == 0) {
                // An OWED slot: its eviction was deferred behind the in-flight job, and now
                // somebody wants the same file again. Adopt the load instead of letting it be
                // thrown away and read a second time. The owed eviction is cancelled, visibly.
                ++stream_counters_.cancelled_evictions;
            }
            ++slot.owners;
            return StreamedAssetHandle<T>{it->second, slot.generation};
        }
        std::uint32_t index;
        if (!pool.free.empty()) {
            // Reuse the most recently evicted index. LIFO keeps the slot deque dense; it is also
            // the order that makes a stale handle most likely to collide with a new tenant, so
            // the generation check below is load-bearing rather than theoretical.
            index = pool.free.back();
            pool.free.pop_back();
        } else {
            index = static_cast<std::uint32_t>(pool.slots.size());
            pool.slots.emplace_back();
        }
        StreamSlot<T>& slot = pool.slots[index];
        slot.state = AssetState::Loading;
        slot.owners = 1;
        slot.occupied = true;
        slot.in_flight = true; // set under the same lock that publishes the slot: pinned from birth
        slot.key = key;
        pool.by_path.emplace(key, index);
        ++pool.live;
        ++stream_counters_.loads_started;
        handle = StreamedAssetHandle<T>{index, slot.generation};
    }
    jobs_.run([this, index = handle.index, path] { load_streamed_job<T>(index, path); },
              &inflight_);
    return handle;
}

template <class T>
void AssetServer::load_streamed_job(std::uint32_t index, std::filesystem::path path) {
    physical_loads_.fetch_add(1, std::memory_order_relaxed);
    // IO + decode OUTSIDE the lock, as for the retained kinds. `asset` is declared before the lock
    // below, so if the result turns out to be unwanted it is destroyed after the lock is dropped.
    std::optional<T> asset;
    if (const std::optional<std::vector<std::byte>> bytes = platform::read_file(path)) {
        AssetError error = AssetError::Io;
        asset = StreamKind<T>::read(*bytes, error);
        if (!asset) {
            RIME_ERROR("assets: async {} load '{}' failed: {}",
                       StreamKind<T>::kName,
                       path.string(),
                       to_string(error));
        }
    } else {
        RIME_ERROR("assets: async {} load cannot open '{}'", StreamKind<T>::kName, path.string());
    }

    StreamPool<T>& pool = stream_pool<T>();
    std::lock_guard<std::mutex> lock(mu_);
    // The index is still ours: in_flight pinned the slot, so it was neither evicted nor reused
    // while we were decoding, however many owners came and went. That is why the job needs no
    // generation of its own.
    StreamSlot<T>& slot = pool.slots[index];
    slot.in_flight = false;
    if (!asset) {
        ++stream_counters_.failed_loads;
    }
    if (slot.owners == 0) {
        // OWED: every owner released while we were loading. This is the deferred eviction being
        // paid — here, by the job, at the first moment it is safe. The decoded bytes never enter
        // the slot. (evict_locked returns the slot's payload; an OWED slot never had one.)
        (void)evict_locked(pool, index);
    } else if (asset) {
        // Park the payload in the slot but leave the state Loading: only pump(), on the main
        // thread, makes it Ready, so a getter never sees a payload appear mid-frame.
        slot.asset = std::move(*asset);
        asset.reset();
        ++pool.resident;
        pool.bytes += StreamKind<T>::bytes(*slot.asset);
        pool.done.push_back(index);
    } else {
        slot.state = AssetState::Failed; // owned and failed: stays Failed until released
    }
}

template <class T>
const AssetServer::StreamSlot<T>* AssetServer::resolve_locked(StreamedAssetHandle<T> handle) const {
    const StreamPool<T>& pool = stream_pool<T>();
    if (handle.index >= pool.slots.size()) {
        return nullptr; // invalid / never issued: "no handle", not a stale one
    }
    const StreamSlot<T>& slot = pool.slots[handle.index];
    // THE GENERATION CHECK. The index says where to look; the generation says whether what is
    // there is still what this handle was issued for. `owners == 0` additionally refuses a handle
    // whose every ownership was released while the load was in flight (an OWED slot): same
    // generation, but nothing the caller is entitled to.
    if (!slot.occupied || slot.generation != handle.generation || slot.owners == 0) {
        ++stream_counters_.stale_handle_resolutions;
        return nullptr;
    }
    return &slot;
}

template <class T>
std::optional<T> AssetServer::evict_locked(StreamPool<T>& pool, std::uint32_t index) {
    StreamSlot<T>& slot = pool.slots[index];
    std::optional<T> payload = std::move(slot.asset);
    slot.asset.reset();
    if (payload) {
        --pool.resident;
        pool.bytes -= StreamKind<T>::bytes(*payload);
        // A payload that was loaded but never pumped is still queued for promotion; take it out,
        // so pump() can never promote an index that now belongs to someone else. (The queue holds
        // at most one frame's completions, so the linear erase is cheap.)
        std::erase(pool.done, index);
    }
    pool.by_path.erase(slot.key);
    slot.key.clear();
    slot.state = AssetState::Loading;
    slot.owners = 0;
    slot.occupied = false;
    slot.in_flight = false;
    // Bump the generation: every handle issued so far for this index is now stale. A u32 wraps
    // after 2^32 evictions of ONE index — at an eviction a second, 136 years — at which point a
    // handle held across the entire wrap could alias. Accepted, and stated.
    ++slot.generation;
    pool.free.push_back(index);
    --pool.live;
    ++stream_counters_.evictions;
    return payload;
}

template <class T> bool AssetServer::release_streamed(StreamedAssetHandle<T> handle) {
    StreamPool<T>& pool = stream_pool<T>();
    // Declared before the lock so an evicted payload is destroyed after the lock is dropped.
    std::optional<T> doomed;
    std::lock_guard<std::mutex> lock(mu_);
    if (resolve_locked(handle) == nullptr) {
        return false; // stale (counted) or invalid: never touches whoever lives there now
    }
    StreamSlot<T>& slot = pool.slots[handle.index];
    if (--slot.owners > 0) {
        return true; // another owner still holds it: stays resident
    }
    if (slot.in_flight) {
        // The load job is running (or queued) and will write into this slot by index. Freeing the
        // slot now would let a new request reuse the index under that job. So the eviction is
        // only RECORDED as owed; load_streamed_job pays it when it publishes.
        ++stream_counters_.deferred_evictions;
        return true;
    }
    doomed = evict_locked(pool, handle.index);
    return true;
}

template <class T> AssetState AssetServer::state_streamed(StreamedAssetHandle<T> handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (handle.index >= stream_pool<T>().slots.size()) {
        return AssetState::Failed; // invalid handle — same answer as the retained kinds
    }
    const StreamSlot<T>* slot = resolve_locked(handle);
    return slot ? slot->state : AssetState::Stale;
}

template <class T> const T* AssetServer::get_streamed(StreamedAssetHandle<T> handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    const StreamSlot<T>* slot = resolve_locked(handle);
    if (slot == nullptr || slot->state != AssetState::Ready) {
        return nullptr;
    }
    // Stable address (deque) and not mutated while Ready; valid until the caller's ownership is
    // released — see the header.
    return &*slot->asset;
}

HeightfieldAssetHandle AssetServer::request_heightfield(const std::filesystem::path& path) {
    return request_streamed<HeightfieldAsset>(path);
}

TerrainLayerAssetHandle AssetServer::request_terrain_layer(const std::filesystem::path& path) {
    return request_streamed<TerrainLayerAsset>(path);
}

StreamedTextureAssetHandle
AssetServer::request_streamed_texture(const std::filesystem::path& path) {
    return request_streamed<TextureAsset>(path);
}

bool AssetServer::release(HeightfieldAssetHandle handle) {
    return release_streamed(handle);
}

bool AssetServer::release(TerrainLayerAssetHandle handle) {
    return release_streamed(handle);
}

bool AssetServer::release(StreamedTextureAssetHandle handle) {
    return release_streamed(handle);
}

AssetState AssetServer::state(HeightfieldAssetHandle handle) const {
    return state_streamed(handle);
}

AssetState AssetServer::state(TerrainLayerAssetHandle handle) const {
    return state_streamed(handle);
}

AssetState AssetServer::state(StreamedTextureAssetHandle handle) const {
    return state_streamed(handle);
}

const HeightfieldAsset* AssetServer::get(HeightfieldAssetHandle handle) const {
    return get_streamed(handle);
}

const TerrainLayerAsset* AssetServer::get(TerrainLayerAssetHandle handle) const {
    return get_streamed(handle);
}

const TextureAsset* AssetServer::get(StreamedTextureAssetHandle handle) const {
    return get_streamed(handle);
}

StreamCounters AssetServer::stream_counters() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stream_counters_;
}

std::size_t AssetServer::live_heightfield_slots() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hf_pool_.live;
}

std::size_t AssetServer::resident_heightfields() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hf_pool_.resident;
}

std::size_t AssetServer::live_terrain_layer_slots() const {
    std::lock_guard<std::mutex> lock(mu_);
    return layer_pool_.live;
}

std::size_t AssetServer::resident_terrain_layers() const {
    std::lock_guard<std::mutex> lock(mu_);
    return layer_pool_.resident;
}

std::size_t AssetServer::live_streamed_texture_slots() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stex_pool_.live;
}

std::size_t AssetServer::resident_streamed_textures() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stex_pool_.resident;
}

std::uint64_t AssetServer::resident_streamed_bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hf_pool_.bytes + layer_pool_.bytes + stex_pool_.bytes;
}

std::uint64_t AssetServer::resident_streamed_texture_bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stex_pool_.bytes;
}

std::size_t AssetServer::pump() {
    std::vector<std::pair<std::uint32_t, MeshAsset>> meshes;
    std::vector<std::pair<std::uint32_t, TextureAsset>> textures;
    std::vector<std::pair<std::uint32_t, MaterialAsset>> materials;
    std::size_t streamed = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        meshes.swap(mesh_done_);
        textures.swap(tex_done_);
        materials.swap(mat_done_);
        // Move each completed CPU load into its slot and mark it Ready, still under the lock so a
        // concurrent job's state write and a getter's read stay ordered.
        for (auto& [index, mesh] : meshes) {
            mesh_slots_[index].asset = std::move(mesh);
            mesh_slots_[index].state = AssetState::Ready;
        }
        for (auto& [index, tex] : textures) {
            tex_slots_[index].asset = std::move(tex);
            tex_slots_[index].state = AssetState::Ready;
        }
        for (auto& [index, mat] : materials) {
            mat_slots_[index].asset = std::move(mat);
            mat_slots_[index].state = AssetState::Ready;
        }
        // Streamed kinds: the payload is already in its slot (the job parked it there); promoting
        // is just the state flip. Every queued index is live and loaded — eviction removes its
        // own entry from the queue — so there is no stale completion to skip here.
        const auto promote = [&streamed](auto& pool) {
            for (const std::uint32_t index : pool.done) {
                pool.slots[index].state = AssetState::Ready;
            }
            streamed += pool.done.size();
            pool.done.clear();
        };
        promote(hf_pool_);
        promote(layer_pool_);
        promote(stex_pool_);
    }
    return meshes.size() + textures.size() + materials.size() + streamed;
}

AssetState AssetServer::state(MeshAssetHandle handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    return handle.index < mesh_slots_.size() ? mesh_slots_[handle.index].state : AssetState::Failed;
}

AssetState AssetServer::state(TextureAssetHandle handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    return handle.index < tex_slots_.size() ? tex_slots_[handle.index].state : AssetState::Failed;
}

AssetState AssetServer::state(MaterialAssetHandle handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    return handle.index < mat_slots_.size() ? mat_slots_[handle.index].state : AssetState::Failed;
}

const MeshAsset* AssetServer::get(MeshAssetHandle handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (handle.index >= mesh_slots_.size() ||
        mesh_slots_[handle.index].state != AssetState::Ready) {
        return nullptr;
    }
    // The slot lives in a deque (stable address) and is never mutated after Ready, so the pointer
    // outlives the lock — the same "hold the handle" contract the registry documents.
    return &*mesh_slots_[handle.index].asset;
}

const TextureAsset* AssetServer::get(TextureAssetHandle handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (handle.index >= tex_slots_.size() || tex_slots_[handle.index].state != AssetState::Ready) {
        return nullptr;
    }
    return &*tex_slots_[handle.index].asset;
}

const MaterialAsset* AssetServer::get(MaterialAssetHandle handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (handle.index >= mat_slots_.size() || mat_slots_[handle.index].state != AssetState::Ready) {
        return nullptr;
    }
    return &*mat_slots_[handle.index].asset;
}

const MeshAsset& AssetServer::get_or_placeholder(MeshAssetHandle handle) const {
    const MeshAsset* mesh = get(handle);
    return mesh ? *mesh : placeholder_mesh_;
}

const TextureAsset& AssetServer::get_or_placeholder(TextureAssetHandle handle) const {
    const TextureAsset* tex = get(handle);
    return tex ? *tex : placeholder_texture_;
}

} // namespace rime::assets
