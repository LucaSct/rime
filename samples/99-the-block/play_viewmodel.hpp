// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "rime/assets/asset_server.hpp"
#include "rime/assets/manifest.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/render/gpu_asset_bridge.hpp"
#include "rime/render/passes.hpp"

namespace rime::core {
class JobSystem;
}

namespace rime::block_play {

// Play-only presentation. The meshes, materials and texture uploads are fixed at load time;
// render_frame only changes a borrowed DrawItem span's world matrices.
class PlayViewmodel {
public:
    enum class Weapon : std::uint8_t { Scar = 0, M4 = 1 };

    PlayViewmodel() = default;
    PlayViewmodel(const PlayViewmodel&) = delete;
    PlayViewmodel& operator=(const PlayViewmodel&) = delete;

    [[nodiscard]] bool load(const std::filesystem::path& cooked,
                            core::JobSystem& jobs,
                            rhi::Device& device,
                            render::MeshRegistry& meshes,
                            render::MaterialRegistry& materials);

    void select(Weapon weapon) noexcept;
    void advance(float frame_dt, core::Vec3 actual_velocity, bool fired) noexcept;
    void observe_camera(core::Vec3 eye) noexcept;
    [[nodiscard]] std::span<const render::DrawItem> pose(const core::Transform& camera);

    [[nodiscard]] Weapon selected() const noexcept { return selected_; }

    [[nodiscard]] std::uint64_t identity_epoch() const noexcept { return identity_epoch_; }

    [[nodiscard]] std::size_t selected_draw_count() const noexcept;
    [[nodiscard]] render::MeshId selected_mesh() const noexcept;

    [[nodiscard]] std::size_t resident_meshes() const noexcept { return resident_meshes_; }

    [[nodiscard]] std::size_t resident_materials() const noexcept { return resident_materials_; }

    [[nodiscard]] std::size_t resident_textures() const noexcept { return resident_textures_; }

private:
    struct Part {
        render::MeshId mesh = render::kInvalidMeshId;
        render::MaterialId material = render::kInvalidMaterialId;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
    };

    struct Model {
        std::vector<Part> parts;
    };

    std::optional<assets::Manifest> manifest_;
    std::unique_ptr<assets::AssetServer> server_;
    std::unique_ptr<render::GpuAssetBridge> bridge_;
    Model models_[2];
    std::vector<render::DrawItem> draws_;
    Weapon selected_ = Weapon::Scar;
    std::uint64_t identity_epoch_ = 1;
    core::Vec3 previous_eye_{};
    bool have_eye_ = false;
    float gait_phase_ = 0.0f;
    float bob_amount_ = 0.0f;
    float recoil_ = 0.0f;
    std::size_t resident_meshes_ = 0;
    std::size_t resident_materials_ = 0;
    std::size_t resident_textures_ = 0;
};

} // namespace rime::block_play
