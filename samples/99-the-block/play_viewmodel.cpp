// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_viewmodel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "rime/assets/cooked_reader.hpp"
#include "rime/core/jobs/job_system.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/platform/input.hpp"

namespace rime::block_play {
namespace {

constexpr std::array<const char*, 6> kSources{"held-scar-h.glb",
                                              "held-m4a1.glb",
                                              "held-msr.glb",
                                              "arms-scar-h.glb",
                                              "arms-m4a1.glb",
                                              "arms-msr.glb"};
constexpr std::size_t kMaxDraws = 32; // SceneRenderer's first-person draw contract

struct TextureRefs {
    assets::TextureAssetHandle base{};
    assets::TextureAssetHandle mr{};
    assets::TextureAssetHandle normal{};
    assets::TextureAssetHandle occlusion{};
    assets::TextureAssetHandle emissive{};
};

struct PendingMaterial {
    assets::MaterialAsset cooked;
    TextureRefs textures;
};

const assets::ManifestEntry* mesh_entry(const assets::Manifest& manifest, const char* filename) {
    for (const auto& entry : manifest.entries()) {
        if (entry.kind == assets::AssetKind::Mesh &&
            std::filesystem::path(entry.source_path).filename() == filename) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

bool PlayViewmodel::load(const std::filesystem::path& cooked,
                         core::JobSystem& jobs,
                         rhi::Device& device,
                         render::MeshRegistry& meshes,
                         render::MaterialRegistry& materials) {
    const auto bytes = platform::read_file(cooked / "manifest.txt");
    if (!bytes) {
        std::fprintf(
            stderr, "99-the-block: viewmodel cook missing under %s\n", cooked.string().c_str());
        return false;
    }
    manifest_ =
        assets::Manifest::parse({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    if (!manifest_) {
        std::fprintf(stderr, "99-the-block: malformed viewmodel manifest\n");
        return false;
    }
    server_ = std::make_unique<assets::AssetServer>(jobs);
    bridge_ = std::make_unique<render::GpuAssetBridge>(device, *server_);
    bridge_->set_catalog(*manifest_, cooked);

    std::array<const assets::ManifestEntry*, 6> entries{};
    std::array<assets::MeshAssetHandle, 6> requests{};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i] = mesh_entry(*manifest_, kSources[i]);
        if (!entries[i]) {
            std::fprintf(stderr, "99-the-block: viewmodel %s has no cooked mesh\n", kSources[i]);
            return false;
        }
        requests[i] = server_->request_mesh(cooked / entries[i]->cooked_file);
    }
    server_->wait_for_pending_loads();
    server_->pump();

    std::unordered_set<std::uint64_t> expected_textures;
    std::array<std::vector<PendingMaterial>, 6> pending;
    bool valid_textures = true;
    const auto request_texture = [&](assets::AssetId id, assets::TextureAssetHandle& out) {
        if (!id.is_valid())
            return;
        const auto* entry = manifest_->find_by_id(id);
        if (!entry || entry->kind != assets::AssetKind::Texture) {
            valid_textures = false;
            return;
        }
        out = bridge_->request_texture(cooked / entry->cooked_file);
        valid_textures = valid_textures && out.is_valid();
        expected_textures.insert(id.value);
    };

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const assets::MeshAsset* mesh = server_->get(requests[i]);
        if (!mesh || mesh->submeshes.empty() || mesh->submeshes.size() > kMaxDraws) {
            std::fprintf(
                stderr, "99-the-block: viewmodel %s has missing/oversized geometry\n", kSources[i]);
            return false;
        }
        const render::MeshId mesh_id = meshes.add(render::mesh_from_cooked(*mesh), kSources[i]);
        if (!meshes.contains(mesh_id))
            return false;
        ++resident_meshes_;
        if (i >= 3) {
            auto& model = models_[i - 3];
            const auto* skeleton_entry =
                manifest_->find_by_source(entries[i]->source_path + "#skeleton");
            const auto skeleton_bytes =
                skeleton_entry ? platform::read_file(cooked / skeleton_entry->cooked_file)
                               : std::nullopt;
            assets::AssetError error{};
            const auto skeleton =
                skeleton_bytes ? assets::read_skeleton(*skeleton_bytes, error) : std::nullopt;
            if (!skeleton || !model.arms.initialize(*mesh, *skeleton)) {
                std::fprintf(stderr, "99-the-block: invalid weighted arm rig %s\n", kSources[i]);
                return false;
            }
            model.arm_mesh = mesh_id;
            model.grips = model.arms.bind_wrists();
            model.previous.resize(mesh->vertex_count);
            model.last.resize(mesh->vertex_count);
        }
        const auto& ranges = meshes.get(mesh_id).submeshes;
        if (ranges.size() != mesh->submeshes.size())
            return false;
        int normal_mapped = 0;
        int metal_rough_mapped = 0;
        for (const auto& range : ranges) {
            const std::string source =
                entries[i]->source_path + "#material" + std::to_string(range.material_slot);
            const auto* material_entry = manifest_->find_by_source(source);
            if (!material_entry || material_entry->kind != assets::AssetKind::Material) {
                std::fprintf(
                    stderr, "99-the-block: viewmodel material missing: %s\n", source.c_str());
                return false;
            }
            const auto material_bytes = platform::read_file(cooked / material_entry->cooked_file);
            assets::AssetError error{};
            const auto material =
                material_bytes ? assets::read_material(*material_bytes, error) : std::nullopt;
            if (!material)
                return false;
            PendingMaterial p;
            p.cooked = *material;
            normal_mapped += material->normal_tex.is_valid() ? 1 : 0;
            metal_rough_mapped += material->metallic_roughness_tex.is_valid() ? 1 : 0;
            request_texture(material->base_color_tex, p.textures.base);
            request_texture(material->metallic_roughness_tex, p.textures.mr);
            request_texture(material->normal_tex, p.textures.normal);
            request_texture(material->occlusion_tex, p.textures.occlusion);
            request_texture(material->emissive_tex, p.textures.emissive);
            pending[i].push_back(p);
            models_[i % 3].parts.push_back({mesh_id,
                                            render::kInvalidMaterialId,
                                            range.first_index,
                                            range.index_count,
                                            i >= 3});
        }
        // The CC-BY MSR is a solid-material model; retain those source materials
        // honestly. The other weapons and every arm still require their native maps.
        if (i != 2 && (normal_mapped == 0 || metal_rough_mapped == 0)) {
            std::fprintf(
                stderr, "99-the-block: %s needs real normal and metal-rough maps\n", kSources[i]);
            return false;
        }
    }
    if (!valid_textures) {
        std::fprintf(stderr, "99-the-block: viewmodel material references unknown textures\n");
        return false;
    }
    server_->wait_for_pending_loads();
    server_->pump();
    (void)bridge_->drain();
    if (bridge_->textures_refused_unsupported() != 0 ||
        bridge_->uploaded_count() != expected_textures.size()) {
        std::fprintf(stderr, "99-the-block: viewmodel textures did not become GPU resident\n");
        return false;
    }
    resident_textures_ = bridge_->uploaded_count();

    const auto ready = [&](assets::TextureAssetHandle h) {
        return h.is_valid() ? bridge_->texture_or_placeholder(h) : rhi::TextureHandle{};
    };
    for (std::size_t i = 0; i < pending.size(); ++i) {
        for (std::size_t j = 0; j < pending[i].size(); ++j) {
            const auto& p = pending[i][j];
            auto desc = render::material_from_cooked(p.cooked);
            desc.base_color_texture = ready(p.textures.base);
            desc.metallic_roughness_texture = ready(p.textures.mr);
            desc.normal_texture = ready(p.textures.normal);
            desc.occlusion_texture = ready(p.textures.occlusion);
            desc.emissive_texture = ready(p.textures.emissive);
            const std::size_t part_offset = i >= 3 ? pending[i - 3].size() : 0;
            models_[i % 3].parts[part_offset + j].material = materials.add(desc);
            ++resident_materials_;
        }
    }
    draws_.reserve(kMaxDraws);
    deformations_.reserve(1);
    // Ocular geometry participates in the SAME model depth and velocity passes as
    // the housing. The optical pass shades this physical surface before main TAA.
    render::CpuMesh lens;
    constexpr unsigned segments = 64;
    const core::Vec3 ocular{.14f, -.118f, -.085f};
    constexpr float radius = kScopeLensRadius;
    lens.vertices.push_back({ocular.x, ocular.y, ocular.z, 0, 0, 1, .5f, .5f});
    for (unsigned i = 0; i <= segments; ++i) {
        const float angle = static_cast<float>(i) * 6.28318530718f / segments;
        const float x = std::cos(angle), y = std::sin(angle);
        lens.vertices.push_back({ocular.x + radius * x,
                                 ocular.y + radius * y,
                                 ocular.z,
                                 0,
                                 0,
                                 1,
                                 .5f + .5f * x,
                                 .5f + .5f * y});
        if (i > 0)
            lens.indices.insert(lens.indices.end(), {0, i, i + 1});
    }
    lens_.mesh = meshes.add(lens, "MSR PM II ocular");
    if (!meshes.contains(lens_.mesh))
        return false;
    ++resident_meshes_;
    render::PbrMaterialDesc glass{};
    glass.base_color[0] = .012f;
    glass.base_color[1] = .018f;
    glass.base_color[2] = .02f;
    glass.roughness = .12f;
    lens_.material = materials.add(glass);
    ++resident_materials_;
    lens_.index_count = static_cast<std::uint32_t>(lens.indices.size());
    return true;
}

void PlayViewmodel::select(Weapon weapon) noexcept {
    if (weapon != selected_) {
        selected_ = weapon;
        ++identity_epoch_;
        gait_phase_ = 0.0f;
        recoil_ = 0.0f;
        aim_ = 0.0f;
        for (auto& model : models_)
            model.history = false;
    }
}

void PlayViewmodel::apply_controls(const platform::Input& devices) noexcept {
    if (devices.key_pressed(platform::Key::Num1))
        select(Weapon::Scar);
    if (devices.key_pressed(platform::Key::Num2))
        select(Weapon::M4);
    if (devices.key_pressed(platform::Key::Num3))
        select(Weapon::Sniper);
    set_aiming(devices.key_down(platform::Key::Q) ||
               devices.mouse_down(platform::MouseButton::Right));
}

float PlayViewmodel::scope_fov_y() noexcept {
    // The rendered feed is fitted into a physical aperture, not across the whole viewport.
    // At ADS, angular magnification = (ocular radius / eye relief) / tan(feed half-FOV).
    // Dividing the PARENT camera's focal length by five ignored the aperture's apparent size
    // and produced only about 2.36x apparent zoom for the authored 26 mm / 90 mm ocular.
    return 2.0f * std::atan(kScopeLensRadius / (kScopeEyeRelief * kScopeMagnification));
}

void PlayViewmodel::advance(float frame_dt, core::Vec3 actual_velocity, bool fired) noexcept {
    const float dt = std::clamp(frame_dt, 0.0f, 0.05f);
    const float speed =
        std::sqrt(actual_velocity.x * actual_velocity.x + actual_velocity.z * actual_velocity.z);
    gait_phase_ += std::min(speed, 10.0f) * dt * 1.6f;
    bob_amount_ = std::clamp(speed / 6.0f, 0.0f, 1.0f);
    if (fired)
        recoil_ = std::min(recoil_ + 0.012f, 0.035f);
    recoil_ *= std::exp(-14.0f * dt);
    const float target_aim = aiming_ ? 1.0f : 0.0f;
    aim_ += (target_aim - aim_) * (1.0f - std::exp(-16.0f * dt));
}

void PlayViewmodel::observe_camera(core::Vec3 eye) noexcept {
    // A correction/teleport must invalidate first-person temporal history even if the weapon did
    // not change. Ordinary 60 Hz walking never travels two metres between rendered frames.
    if (have_eye_ && core::length(eye - previous_eye_) > 2.0f) {
        ++identity_epoch_;
        gait_phase_ = 0.0f;
        recoil_ = 0.0f;
        for (auto& model : models_)
            model.history = false;
    }
    previous_eye_ = eye;
    have_eye_ = true;
}

std::span<const render::DrawItem> PlayViewmodel::pose(const core::Transform& camera) {
    draws_.clear();
    core::Transform local{};
    local.translation = {0.006f * std::sin(gait_phase_) * bob_amount_,
                         0.008f * std::sin(2.0f * gait_phase_) * bob_amount_ - recoil_,
                         recoil_ * 0.5f};
    const float pose_amount = 1.0f - aim_ * .85f;
    local.translation = local.translation * pose_amount;
    // These weapon-local sight lines are authored in metres: SCAR rear iron, M4 sight tube,
    // and PM II ocular. Translate each line onto the eye without changing the world camera.
    // Rifle aim has no magnification; only the sniper owns a second optical camera.
    constexpr std::array<core::Vec3, 3> aim_offsets{
        {{-.14f, .09f, .06f}, {-.1378f, .1097f, .06f}, {-.14f, .118f, -.005f}}};
    local.translation = local.translation + aim_offsets[static_cast<unsigned>(selected_)] * aim_;
    local.rotation = core::quat_from_euler(
        recoil_ * .8f, .008f * std::sin(gait_phase_ * .5f) * bob_amount_ * pose_amount, 0.0f);
    const core::Mat4 camera_matrix = core::to_matrix(camera);
    const core::Mat4 weapon_local = core::to_matrix(local);
    const core::Mat4 model = camera_matrix * weapon_local;
    auto& selected = models_[static_cast<unsigned>(selected_)];
    const auto vertices = selected.arms.solve(
        weapon_local, selected.grips, {{{-.4f, -.65f, -.25f}, {.5f, -.65f, -.25f}}});
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const core::Vec3 current{vertices[i].px, vertices[i].py, vertices[i].pz};
        selected.previous[i] = selected.history ? selected.last[i] : current;
        selected.last[i] = current;
    }
    selected.history = true;
    deformations_.clear();
    deformations_.push_back({selected.arm_mesh, vertices, selected.previous});
    for (const auto& part : selected.parts) {
        render::DrawItem draw{};
        draw.mesh = part.mesh;
        draw.material = part.material;
        draw.model = part.arm ? camera_matrix : model;
        draw.first_index = part.first_index;
        draw.index_count = part.index_count;
        draws_.push_back(draw);
    }
    if (scoped()) {
        lens_.model = model;
        draws_.push_back(lens_);
        optic_world_ = model * core::mat4_translation({.14f, -.118f, -.085f});
    }
    return draws_;
}

std::size_t PlayViewmodel::selected_draw_count() const noexcept {
    return models_[static_cast<unsigned>(selected_)].parts.size() + (scoped() ? 1 : 0);
}

render::MeshId PlayViewmodel::selected_mesh() const noexcept {
    const auto& parts = models_[static_cast<unsigned>(selected_)].parts;
    return parts.empty() ? render::kInvalidMeshId : parts.front().mesh;
}

} // namespace rime::block_play
