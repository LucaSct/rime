// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/assets/registry.hpp"

#include <utility>

#include "rime/core/diagnostics/log.hpp"
#include "rime/platform/filesystem.hpp"

namespace rime::assets {

MeshHandle AssetRegistry::load_mesh_from_memory(std::span<const std::byte> file,
                                                AssetError& out_error) {
    // Validate the envelope and split off the payload, then take the content hash *before*
    // decoding: if this exact content is already loaded, we return the existing handle and skip the
    // decode entirely — content addressing is what makes a repeated load (the cook cache /
    // duplicate reference case) essentially free. The kind/schema checks mirror read_mesh's;
    // keeping them here is what lets us reach the id without first decoding a mesh we might already
    // hold.
    std::span<const std::byte> payload;
    const std::optional<CookedHeader> header = read_header(file, payload, out_error);
    if (!header) {
        return MeshHandle{};
    }
    if (header->kind != AssetKind::Mesh) {
        out_error = AssetError::WrongKind;
        return MeshHandle{};
    }
    if (header->type_schema_hash != mesh_schema_hash()) {
        out_error = AssetError::SchemaMismatch;
        return MeshHandle{};
    }

    const AssetId id = content_hash(payload);
    if (const auto it = mesh_by_id_.find(id.value); it != mesh_by_id_.end()) {
        return it->second;
    }

    std::optional<MeshAsset> mesh = decode_mesh(payload, out_error);
    if (!mesh) {
        return MeshHandle{};
    }

    const MeshHandle handle = meshes_.insert(std::move(*mesh));
    mesh_by_id_.emplace(id.value, handle);
    return handle;
}

MeshHandle AssetRegistry::load_mesh(const std::filesystem::path& path) {
    const std::optional<std::vector<std::byte>> bytes = platform::read_file(path);
    if (!bytes) {
        RIME_ERROR("assets: cannot open mesh file '{}'", path.string());
        return MeshHandle{};
    }

    AssetError error = AssetError::Io;
    const MeshHandle handle = load_mesh_from_memory(*bytes, error);
    if (!handle.is_valid()) {
        RIME_ERROR("assets: failed to load mesh '{}': {}", path.string(), to_string(error));
    }
    return handle;
}

const MeshAsset* AssetRegistry::get(MeshHandle handle) const noexcept {
    return meshes_.get(handle);
}

} // namespace rime::assets
