// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "play_arm_ik.hpp"
#include "play_weapon_motion.hpp"
#include "rime/assets/asset_server.hpp"
#include "rime/assets/manifest.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/render/gpu_asset_bridge.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/scene_renderer.hpp"

namespace rime::core {
class JobSystem;
}

namespace rime::platform {
class Input;
struct Event;
} // namespace rime::platform

namespace rime::block_play {

// Play-only presentation. Mesh topology/materials/textures load once. Each render solves
// camera-attached arm chains and borrows their skinned vertices through the renderer's copy.
class PlayViewmodel {
public:
    enum class Weapon : std::uint8_t { Scar = 0, M4 = 1, Sniper = 2 };

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

    [[nodiscard]] bool begin_reload() noexcept {
        return motion_.begin_reload(static_cast<PlayWeaponMotion::Weapon>(selected_));
    }

    [[nodiscard]] bool reloading() const noexcept { return motion_.reloading(); }

    [[nodiscard]] bool cycling() const noexcept { return motion_.cycling(); }

    [[nodiscard]] bool busy() const noexcept { return motion_.busy(); }

    [[nodiscard]] bool take_case_ejection() noexcept { return motion_.take_case_ejection(); }

    // Consumes the already-folded frame levels/selection edges. A right drag can look and aim
    // together when pointer capture is unavailable; capture must never disable the aim button.
    void apply_controls(const platform::Input& devices,
                        std::span<const platform::Event> events = {}) noexcept;

    void set_aiming(bool aiming) noexcept { aiming_ = aiming; }

    static constexpr float kScopeLensRadius = 0.026f;
    static constexpr float kScopeEyeRelief = 0.05f;
    static constexpr float kScopeMagnification = 5.0f;
    static constexpr float kScopeEyeBoxRadius = 0.012f;
    static constexpr float kScopeEyeReliefTolerance = 0.06f;
    [[nodiscard]] static float scope_fov_y() noexcept;

    [[nodiscard]] std::span<const render::DrawItem> pose(const core::Transform& camera);

    [[nodiscard]] Weapon selected() const noexcept { return selected_; }

    [[nodiscard]] std::uint64_t identity_epoch() const noexcept { return identity_epoch_; }

    [[nodiscard]] std::size_t selected_draw_count() const noexcept;
    [[nodiscard]] render::MeshId selected_mesh() const noexcept;

    [[nodiscard]] std::span<const render::FirstPersonDeformation> deformations() const noexcept {
        return deformations_;
    }

    [[nodiscard]] const PlayArmIk::Stats& arm_stats() const noexcept {
        return models_[static_cast<unsigned>(selected_)].arms.stats();
    }

    // What the action hand paths actually used in the LAST pose(): per side (left support,
    // right firing), the fraction of the authored offset kept and the offset applied in weapon
    // metres. limited_poses counts every pose that shortened a path, so a shortened reach is
    // visible to a proof rather than silently absorbed.
    struct ReachStats {
        std::array<float, 2> retained{1.0f, 1.0f};
        std::array<float, 2> applied_offset{};
        std::uint64_t limited_poses = 0;
    };

    [[nodiscard]] const ReachStats& reach_stats() const noexcept { return reach_stats_; }

    // Last posed wrist targets after the physical reach limit, in world metres.
    [[nodiscard]] const std::array<core::Vec3, 2>& wrist_world() const noexcept {
        return wrist_world_;
    }

    [[nodiscard]] std::uint32_t magazine_triangles() const noexcept {
        return models_[static_cast<unsigned>(selected_)].magazine_triangles;
    }

    [[nodiscard]] core::Mat4 magazine_world() const noexcept { return magazine_world_; }

    [[nodiscard]] core::Vec3 magazine_contact_world() const noexcept {
        return magazine_contact_world_;
    }

    [[nodiscard]] float magazine_contact_gap() const noexcept { return magazine_contact_gap_; }

    // Shoulder-to-wrist distance limits of the selected rig, MEASURED at load through the real
    // solver, not re-derived from bone lengths: {near, far} per side, metres.
    [[nodiscard]] std::array<std::array<float, 2>, 2> reach_limits() const noexcept {
        const auto& m = models_[static_cast<unsigned>(selected_)];
        return {{{m.reach_near[0], m.reach_far[0]}, {m.reach_near[1], m.reach_far[1]}}};
    }

    // Distance kept inside each measured limit. See pose() for why 5 mm.
    static constexpr float kReachMargin = 0.005f;

    [[nodiscard]] bool scoped() const noexcept { return selected_ == Weapon::Sniper; }

    [[nodiscard]] float aim_fraction() const noexcept { return aim_; }

    [[nodiscard]] const render::DrawItem& lens() const noexcept {
        return selected_ == Weapon::Scar ? collimated_lens_ : lens_;
    }

    [[nodiscard]] core::Mat4 optic_world() const noexcept { return optic_world_; }

    // Conservative exit-pupil test after pose(): an entirely closed pupil needs reflected
    // glass, but no second rendering of the world. Uses the same limits as scope_lens.frag.
    [[nodiscard]] bool scope_feed_visible(const core::Transform& eye) const noexcept;

    [[nodiscard]] core::Vec3 muzzle_world(const core::Transform& camera) const noexcept {
        return core::transform_point(core::to_matrix(camera) * weapon_local_,
                                     models_[static_cast<unsigned>(selected_)].muzzle);
    }

    [[nodiscard]] std::size_t resident_meshes() const noexcept { return resident_meshes_; }

    [[nodiscard]] std::size_t resident_materials() const noexcept { return resident_materials_; }

    [[nodiscard]] std::size_t resident_textures() const noexcept { return resident_textures_; }

private:
    render::DrawItem collimated_lens_{};

    struct Part {
        render::MeshId mesh = render::kInvalidMeshId;
        render::MaterialId material = render::kInvalidMaterialId;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        bool arm = false;
        bool magazine = false;
        std::uint8_t source_file = 0;
        std::uint32_t source_range = 0;
    };

    struct Model {
        core::Vec3 muzzle{}, stock{};
        core::Mat4 camera_from_arms{}, arms_from_camera{};
        std::array<core::Vec3, 2> elbow_poles{};
        std::vector<Part> parts;
        PlayArmIk arms;
        render::MeshId arm_mesh = render::kInvalidMeshId;
        std::array<core::Mat4, 2> grips{};
        std::array<float, 2> reach_near{}, reach_far{};
        core::Vec3 magazine_contact{};
        std::uint32_t magazine_triangles = 0;
        std::vector<core::Vec3> previous, last;
        bool history = false;
    };

    std::optional<assets::Manifest> manifest_;
    std::unique_ptr<assets::AssetServer> server_;
    std::unique_ptr<render::GpuAssetBridge> bridge_;
    Model models_[3];
    std::vector<render::DrawItem> draws_;
    Weapon selected_ = Weapon::Scar;
    std::uint64_t identity_epoch_ = 1;
    core::Vec3 previous_eye_{};
    bool have_eye_ = false;
    float gait_phase_ = 0.0f;
    float bob_amount_ = 0.0f;
    float recoil_ = 0.0f;
    PlayWeaponMotion motion_;
    bool aiming_ = false;
    float aim_ = 0.0f;
    render::DrawItem lens_{};
    core::Mat4 optic_world_{};
    core::Mat4 weapon_local_{};
    std::vector<render::FirstPersonDeformation> deformations_;
    ReachStats reach_stats_{};
    std::array<core::Vec3, 2> wrist_world_{};
    core::Mat4 magazine_world_{};
    core::Vec3 magazine_contact_world_{};
    float magazine_contact_gap_ = 0.0f;
    std::size_t resident_meshes_ = 0;
    std::size_t resident_materials_ = 0;
    std::size_t resident_textures_ = 0;
};

} // namespace rime::block_play
