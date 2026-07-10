// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/assets/asset_server.hpp"

#include <span>

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
    const std::string key = path.string();
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
    const std::string key = path.string();
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

std::size_t AssetServer::pump() {
    std::vector<std::pair<std::uint32_t, MeshAsset>> meshes;
    std::vector<std::pair<std::uint32_t, TextureAsset>> textures;
    {
        std::lock_guard<std::mutex> lock(mu_);
        meshes.swap(mesh_done_);
        textures.swap(tex_done_);
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
    }
    return meshes.size() + textures.size();
}

AssetState AssetServer::state(MeshAssetHandle handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    return handle.index < mesh_slots_.size() ? mesh_slots_[handle.index].state : AssetState::Failed;
}

AssetState AssetServer::state(TextureAssetHandle handle) const {
    std::lock_guard<std::mutex> lock(mu_);
    return handle.index < tex_slots_.size() ? tex_slots_[handle.index].state : AssetState::Failed;
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

const MeshAsset& AssetServer::get_or_placeholder(MeshAssetHandle handle) const {
    const MeshAsset* mesh = get(handle);
    return mesh ? *mesh : placeholder_mesh_;
}

const TextureAsset& AssetServer::get_or_placeholder(TextureAssetHandle handle) const {
    const TextureAsset* tex = get(handle);
    return tex ? *tex : placeholder_texture_;
}

} // namespace rime::assets
