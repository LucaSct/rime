// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <unordered_map>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/mesh_asset.hpp"
#include "rime/core/containers/slot_map.hpp"

// The runtime owner of loaded assets. One registry holds every loaded asset of a kind in a SlotMap
// and hands out cheap generational handles (never raw pointers into relocatable storage). Loading
// is synchronous in M6.1 — read the file, validate it, keep it — with the job-system async path
// layered on at M6.5. Identity does the de-duplication: two loads that resolve to the same content
// hash return the *same* handle and store the bytes once (ADR-0024, decision 2).
//
// Hot-reload is a documented seam, not a feature (decision 10): the handle indirection is exactly
// what a future "re-cook, swap the slot's target, bump its generation" needs, so it can arrive
// without an interface break. Nothing here implements it yet.
namespace rime::assets {

// A generational handle to a mesh owned by an AssetRegistry. Phantom-typed on MeshAsset so it
// cannot be mixed up with other handle kinds.
using MeshHandle = core::Handle<MeshAsset>;

class AssetRegistry {
public:
    AssetRegistry() = default;

    // Owns loaded bytes; non-copyable so ownership is unambiguous. (Movable via the defaults is not
    // needed yet and left off to keep the type's role obvious.)
    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // Load a cooked mesh from a file on disk. Returns an invalid handle on any failure (a missing
    // file, or any AssetError from the reader), having logged what went wrong. A second load of the
    // same content returns the first handle without re-reading the file's payload.
    [[nodiscard]] MeshHandle load_mesh(const std::filesystem::path& path);

    // Load a cooked mesh already resident in memory (a pack entry, a test fixture). Surfaces the
    // specific AssetError in `out_error` so callers — chiefly the M6.1 negative battery — can
    // assert on the exact failure. On success `out_error` is left untouched.
    [[nodiscard]] MeshHandle load_mesh_from_memory(std::span<const std::byte> file,
                                                   AssetError& out_error);

    // The mesh a handle names, or nullptr if the handle is stale/invalid. The pointer is valid only
    // until the next load (the backing store may relocate) — hold the handle, not the pointer.
    [[nodiscard]] const MeshAsset* get(MeshHandle handle) const noexcept;

    [[nodiscard]] std::size_t mesh_count() const noexcept { return meshes_.size(); }

private:
    core::SlotMap<MeshAsset> meshes_;
    // content-hash id -> handle, for load de-duplication. Keyed on the raw u64 so the map needs no
    // custom hash for AssetId.
    std::unordered_map<std::uint64_t, MeshHandle> mesh_by_id_;
};

} // namespace rime::assets
