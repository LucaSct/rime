// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_viewmodel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
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

constexpr std::array<core::Vec3, 3> kAimOffsets{
    {{-.14f, .09f, .37f}, {-.137777f, .0947244f, .37f}, {-.14f, .118f, .035f}}};
constexpr std::array<core::Vec3, 3> kHipOffsets{{{0, 0, .22f}, {0, 0, .22f}, {0, 0, 0}}};
constexpr core::Vec3 kScarSight{.14f, -.0848075f, -.473f};

core::Mat4 sight_alignment(unsigned weapon, float aim) {
    if (weapon != 1)
        return {};
    // The M4's front post is 4.890 mm below its rear peep over a 370.56 mm sight radius.
    // Rotate the whole weapon about that measured peep, aligning BOTH sights with the eye.
    // A translation can centre the aperture alone while leaving the front post misaligned.
    constexpr core::Vec3 peep{.137777f, -.094724381f, -.46498038f};
    return core::mat4_translation(peep) *
           core::to_mat4(core::quat_from_axis_angle({1, 0, 0}, .0131968f * aim)) *
           core::mat4_translation(-peep);
}

bool extract_scar_glass(render::CpuMesh& gun, render::CpuMesh& glass) {
    // The authored holographic sight contains two opaque, isolated glass hulls sharing its
    // housing material. Identify their measured metre-space bounds INSIDE the same submesh:
    // a global front-box test also catches seven housing triangles. Refuse changed artwork
    // rather than silently cutting an unrelated part. Keep the original near glass as the mask.
    const auto pane = [&](std::size_t index, bool rear) {
        for (unsigned j = 0; j < 3; ++j) {
            const auto& v = gun.vertices[gun.indices[index + j]];
            if (!(v.px > .1219f && v.px < .1581f && v.py > -.0967f && v.py < -.0729f &&
                  v.pz > (rear ? -.476f : -.5005f) && v.pz < (rear ? -.4729f : -.4975f)))
                return false;
        }
        return true;
    };
    std::size_t found = gun.submeshes.size();
    for (std::size_t i = 0; i < gun.submeshes.size(); ++i) {
        const auto& range = gun.submeshes[i];
        unsigned rear = 0, front = 0;
        for (std::size_t t = range.first_index; t < range.first_index + range.index_count; t += 3) {
            rear += pane(t, true) ? 1u : 0u;
            front += pane(t, false) ? 1u : 0u;
        }
        if (rear == 316 && front == 316) {
            if (found != gun.submeshes.size())
                return false;
            found = i;
        }
    }
    if (found == gun.submeshes.size())
        return false;
    std::vector<std::uint32_t> kept;
    std::vector<std::uint32_t> remap(gun.vertices.size(),
                                     std::numeric_limits<std::uint32_t>::max());
    for (std::size_t i = 0; i < gun.submeshes.size(); ++i) {
        auto& range = gun.submeshes[i];
        const auto begin = static_cast<std::uint32_t>(kept.size());
        for (std::size_t t = range.first_index; t < range.first_index + range.index_count; t += 3) {
            const bool rear = i == found && pane(t, true);
            const bool front = i == found && pane(t, false);
            for (unsigned j = 0; j < 3; ++j) {
                const auto source = gun.indices[t + j];
                if (rear) {
                    auto& destination = remap[source];
                    if (destination == std::numeric_limits<std::uint32_t>::max()) {
                        destination = static_cast<std::uint32_t>(glass.vertices.size());
                        glass.vertices.push_back(gun.vertices[source]);
                    }
                    glass.indices.push_back(destination);
                } else if (!front) {
                    kept.push_back(source);
                }
            }
        }
        range.first_index = begin;
        range.index_count = static_cast<std::uint32_t>(kept.size()) - begin;
    }
    gun.indices = std::move(kept);
    return !glass.indices.empty();
}

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
        auto geometry = render::mesh_from_cooked(*mesh);
        if (i == 0) {
            render::CpuMesh glass;
            if (!extract_scar_glass(geometry, glass)) {
                std::fprintf(stderr,
                             "99-the-block: SCAR sight glass no longer matches its calibration\n");
                return false;
            }
            collimated_lens_.mesh = meshes.add(glass, "SCAR collimated sight glass");
            if (!meshes.contains(collimated_lens_.mesh))
                return false;
            collimated_lens_.index_count = static_cast<std::uint32_t>(glass.indices.size());
            ++resident_meshes_;
        }
        if (i < 3 && !geometry.vertices.empty()) {
            // Prepared guns point down -Z. Average the frontmost centimetre of the barrel
            // rather than guessing a camera-space flash position for differently sized guns.
            float front = geometry.vertices.front().pz, rear = front;
            for (const auto& v : geometry.vertices) {
                front = std::min(front, v.pz);
                rear = std::max(rear, v.pz);
            }
            const float extent = std::numeric_limits<float>::max();
            core::Vec3 low{extent, extent, extent}, high{-extent, -extent, -extent};
            for (const auto& v : geometry.vertices)
                if (v.pz >= rear - .005f) {
                    low.x = std::min(low.x, v.px);
                    low.y = std::min(low.y, v.py);
                    high.x = std::max(high.x, v.px);
                    high.y = std::max(high.y, v.py);
                }
            models_[i].stock = {(low.x + high.x) * .5f, (low.y + high.y) * .5f, rear};
            core::Vec3 muzzle{};
            unsigned count = 0;
            for (const auto& v : geometry.vertices)
                if (v.pz <= front + .01f) {
                    muzzle = muzzle + core::Vec3{v.px, v.py, v.pz};
                    ++count;
                }
            models_[i].muzzle = muzzle / static_cast<float>(count);
        }
        const render::MeshId mesh_id = meshes.add(geometry, kSources[i]);
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
            // The camera follows the RIGHT aiming eye. Seat the gun's measured butt on its
            // right shoulder and blade the torso 60 degrees: the left shoulder comes forward
            // for the support hand. This reference stays fixed through bob/recoil; solving
            // wrists alone had left the stock floating far from either shoulder.
            const auto shoulder = model.arms.bind_shoulders()[1];
            const auto seat = core::transform_point(
                                  sight_alignment(static_cast<unsigned>(i - 3), 1), model.stock) +
                              kAimOffsets[i - 3];
            model.camera_from_arms =
                core::mat4_translation(seat) *
                core::to_mat4(core::quat_from_axis_angle({0, 1, 0}, -1.0471975512f)) *
                core::mat4_translation(-shoulder);
            model.arms_from_camera = core::inverse(model.camera_from_arms);
            model.elbow_poles = {
                {core::transform_point(model.arms_from_camera, {-.4f, -.65f, -.25f}),
                 core::transform_point(model.arms_from_camera, {.5f, -.65f, -.25f})}};
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
    collimated_lens_.material = lens_.material;
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
        motion_.cancel(static_cast<PlayWeaponMotion::Weapon>(weapon));
        for (auto& model : models_)
            model.history = false;
    }
}

void PlayViewmodel::apply_controls(const platform::Input& devices,
                                   std::span<const platform::Event> events) noexcept {
    if (devices.key_pressed(platform::Key::Num1))
        select(Weapon::Scar);
    if (devices.key_pressed(platform::Key::Num2))
        select(Weapon::M4);
    if (devices.key_pressed(platform::Key::Num3))
        select(Weapon::Sniper);
    // A tap can begin AND end between two rendered frames. The final device level then has
    // no pressed edge; selection follows the real down event so a quick 3 never leaves a rifle
    // active when the player expects the magnified optic. Repeats do not reset the aim pose.
    for (const auto& event : events) {
        if (event.type != platform::EventType::KeyDown || event.key.repeat)
            continue;
        if (event.key.key == platform::Key::Num1)
            select(Weapon::Scar);
        if (event.key.key == platform::Key::Num2)
            select(Weapon::M4);
        if (event.key.key == platform::Key::Num3)
            select(Weapon::Sniper);
        if (event.key.key == platform::Key::R)
            (void)begin_reload();
    }
    if (devices.key_pressed(platform::Key::R))
        (void)begin_reload();
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

bool PlayViewmodel::scope_feed_visible(const core::Transform& eye) const noexcept {
    if (!scoped())
        return false;
    const auto p = core::transform_point(core::inverse(optic_world_), eye.translation);
    // The shader intersects two unit discs: the physical aperture and an exit pupil shifted
    // by eye.xy / eye_box_radius. Their supports cannot meet beyond two radii. A small
    // conservative margin avoids switching at a floating-point boundary during an aim blend.
    constexpr float margin = .0001f;
    const float support = 2 * kScopeEyeBoxRadius + margin;
    return p.z > 0 && std::abs(p.z - kScopeEyeRelief) < kScopeEyeReliefTolerance + margin &&
           p.x * p.x + p.y * p.y < support * support;
}

void PlayViewmodel::advance(float frame_dt, core::Vec3 actual_velocity, bool fired) noexcept {
    // Animation time uses the entire finite frame; a slow frame must not extend a reload.
    const float dt = std::isfinite(frame_dt) && frame_dt >= 0.0f ? frame_dt : 0.0f;
    motion_.advance(frame_dt, static_cast<PlayWeaponMotion::Weapon>(selected_), fired);
    const float speed =
        std::sqrt(actual_velocity.x * actual_velocity.x + actual_velocity.z * actual_velocity.z);
    gait_phase_ += std::min(speed, 10.0f) * dt * 1.6f;
    bob_amount_ = std::clamp(speed / 6.0f, 0.0f, 1.0f);
    if (fired)
        recoil_ = std::min(recoil_ + (scoped() ? 0.028f : 0.012f), 0.06f);
    recoil_ *= std::exp(-14.0f * dt);
    const float target_aim = aiming_ && !motion_.busy() ? 1.0f : 0.0f;
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
    const auto motion_pose = motion_.pose();
    core::Transform local{};
    local.translation = {0.006f * std::sin(gait_phase_) * bob_amount_,
                         0.008f * std::sin(2.0f * gait_phase_) * bob_amount_ - recoil_,
                         recoil_ * 0.5f};
    const float pose_amount = 1.0f - aim_ * .85f;
    local.translation = local.translation * pose_amount;
    // These weapon-local sight lines are authored in metres: SCAR holographic glass, M4 peep,
    // and PM II ocular. Align each line with the eye without changing the world camera.
    // Rifle aim has no magnification; only the sniper owns a second optical camera.
    // A shouldered stock belongs behind the eye. The old rifle pose exposed its rear cap
    // 23 cm IN FRONT of the camera; matching hip placement keeps the entire transition reachable.
    const auto index = static_cast<unsigned>(selected_);
    local.translation =
        local.translation + kAimOffsets[index] * aim_ + kHipOffsets[index] * (1 - aim_);
    // The whole gun lowers and rolls during reload; there is no separate animated magazine.
    local.translation = local.translation + core::Vec3{0.02f * motion_pose.lower,
                                                       -0.12f * motion_pose.lower,
                                                       0.09f * motion_pose.lower};
    local.rotation =
        core::quat_from_euler(recoil_ * .8f + 0.10f * motion_pose.lower,
                              .008f * std::sin(gait_phase_ * .5f) * bob_amount_ * pose_amount,
                              0.22f * motion_pose.tilt);
    const core::Mat4 camera_matrix = core::to_matrix(camera);
    const core::Mat4 weapon_local = core::to_matrix(local) * sight_alignment(index, aim_);
    weapon_local_ = weapon_local;
    const core::Mat4 model = camera_matrix * weapon_local;
    auto& selected = models_[static_cast<unsigned>(selected_)];
    auto grips = selected.grips;
    // Slot zero is the support left hand; slot one is the firing right hand. Moving
    // wrist targets leaves the shoulder anchors fixed and uses the existing CPU IK mesh.
    grips[0] = core::mat4_translation({0.035f * motion_pose.left_magazine,
                                       -0.095f * motion_pose.left_magazine,
                                       0.10f * motion_pose.left_magazine}) *
               grips[0];
    grips[1] =
        core::mat4_translation({0.025f * motion_pose.right_bolt,
                                0.035f * motion_pose.right_bolt,
                                -0.11f * motion_pose.right_bolt + 0.095f * motion_pose.bolt_pull}) *
        grips[1];
    const auto vertices =
        selected.arms.solve(selected.arms_from_camera * weapon_local, grips, selected.elbow_poles);
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
        draw.model = part.arm ? camera_matrix * selected.camera_from_arms : model;
        draw.first_index = part.first_index;
        draw.index_count = part.index_count;
        draws_.push_back(draw);
    }
    if (scoped()) {
        lens_.model = model;
        draws_.push_back(lens_);
        optic_world_ = model * core::mat4_translation({.14f, -.118f, -.085f});
    } else if (selected_ == Weapon::Scar) {
        collimated_lens_.model = model;
        draws_.push_back(collimated_lens_);
        optic_world_ = model * core::mat4_translation(kScarSight);
    }
    return draws_;
}

std::size_t PlayViewmodel::selected_draw_count() const noexcept {
    return models_[static_cast<unsigned>(selected_)].parts.size() +
           ((scoped() || selected_ == Weapon::Scar) ? 1 : 0);
}

render::MeshId PlayViewmodel::selected_mesh() const noexcept {
    const auto& parts = models_[static_cast<unsigned>(selected_)].parts;
    return parts.empty() ? render::kInvalidMeshId : parts.front().mesh;
}

} // namespace rime::block_play
