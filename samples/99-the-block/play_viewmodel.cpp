// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_viewmodel.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

struct MagazineSplit {
    render::CpuMesh magazine;
    core::Vec3 contact{};
    std::uint32_t triangles = 0;
};

bool split_magazine(render::CpuMesh& body, unsigned weapon, MagazineSplit& out) {
    // Source GLBs measured in prepared weapon-local metres. M4's authored material slot is a
    // clean magazine. SCAR and MSR share materials with their receivers: classify WHOLE
    // index-connected pieces by bounds, never triangles clipped along an arbitrary plane.
    // MSR also gates material slots: slot 7 overlaps its box but is receiver geometry.
    // Exact triangle totals make changed artwork fail closed instead of moving receiver faces.
    constexpr std::array<core::Vec3, 3> lo{
        {{.120f, -.349f, -.620f}, {}, {.1255f, -.308f, -.3282f}}};
    constexpr std::array<core::Vec3, 3> hi{
        {{.157f, -.187f, -.534f}, {}, {.1545f, -.204f, -.2165f}}};
    constexpr std::array<std::uint32_t, 3> expected{1152, 664, 918};
    if (weapon >= expected.size())
        return false;
    std::vector<std::uint32_t> parent(body.vertices.size());
    for (std::uint32_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
    const auto root = [&](std::uint32_t index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    for (std::size_t t = 0; t < body.indices.size(); t += 3) {
        const auto a = root(body.indices[t]);
        parent[root(body.indices[t + 1])] = a;
        parent[root(body.indices[t + 2])] = a;
    }

    struct Bounds {
        core::Vec3 low{std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
        core::Vec3 high{-std::numeric_limits<float>::max(),
                        -std::numeric_limits<float>::max(),
                        -std::numeric_limits<float>::max()};
    };

    std::vector<Bounds> bounds(body.vertices.size());
    for (const auto index : body.indices) {
        auto& b = bounds[root(index)];
        const auto& v = body.vertices[index];
        b.low = {std::min(b.low.x, v.px), std::min(b.low.y, v.py), std::min(b.low.z, v.pz)};
        b.high = {std::max(b.high.x, v.px), std::max(b.high.y, v.py), std::max(b.high.z, v.pz)};
    }
    const auto inside = [&](std::uint32_t index) {
        const auto& b = bounds[root(index)];
        return b.low.x >= lo[weapon].x && b.low.y >= lo[weapon].y && b.low.z >= lo[weapon].z &&
               b.high.x <= hi[weapon].x && b.high.y <= hi[weapon].y && b.high.z <= hi[weapon].z;
    };
    out.magazine.vertices = body.vertices; // preserve positions, normals, UVs and tangents
    std::vector<std::uint32_t> kept;
    std::array<std::uint32_t, 12> magazine_by_material{};
    core::Vec3 contact_sum{};
    std::uint32_t contact_vertices = 0;
    for (auto& range : body.submeshes) {
        const auto body_begin = static_cast<std::uint32_t>(kept.size());
        const auto mag_begin = static_cast<std::uint32_t>(out.magazine.indices.size());
        for (std::size_t t = range.first_index; t < range.first_index + range.index_count; t += 3) {
            const bool msr_material = range.material_slot == 6 || range.material_slot == 8 ||
                                      range.material_slot == 9 || range.material_slot == 10;
            const bool mag = weapon == 1 ? range.material_slot == 2
                                         : inside(body.indices[t]) && (weapon == 0 || msr_material);
            if (mag && range.material_slot < magazine_by_material.size())
                ++magazine_by_material[range.material_slot];
            auto& target = mag ? out.magazine.indices : kept;
            for (unsigned j = 0; j < 3; ++j) {
                const auto source = body.indices[t + j];
                target.push_back(source);
                if (mag) {
                    const auto& v = body.vertices[source];
                    contact_sum = contact_sum + core::Vec3{v.px, v.py, v.pz};
                    ++contact_vertices;
                }
            }
        }
        out.magazine.submeshes.push_back(
            {mag_begin,
             static_cast<std::uint32_t>(out.magazine.indices.size()) - mag_begin,
             range.material_slot});
        range.first_index = body_begin;
        range.index_count = static_cast<std::uint32_t>(kept.size()) - body_begin;
    }
    out.triangles = static_cast<std::uint32_t>(out.magazine.indices.size() / 3);
    if (out.triangles != expected[weapon])
        return false;
    if (weapon == 2 && (magazine_by_material[6] != 282 || magazine_by_material[8] != 156 ||
                        magazine_by_material[9] != 432 || magazine_by_material[10] != 48))
        return false;
    body.indices = std::move(kept);
    // The palm aims at the magazine's centre near its upper socket. The articulated fingers in
    // the source rig are locked; this is a wrist contact target, not a finger-grasp claim.
    out.contact = contact_sum / static_cast<float>(contact_vertices);
    out.contact.y = std::min(out.contact.y + .04f, hi[weapon].y);
    return true;
}

std::uint64_t geometry_fingerprint(const render::CpuMesh& mesh) {
    // Stable FNV-1a over authored topology and position bits, independent of host byte order.
    // The expected hashes are calibrated from cooked assets in a hardware-capable run; until
    // then printing the observed value is evidence, not a same-count art-change guard.
    std::uint64_t hash = 14695981039346656037ull;
    const auto word = [&](std::uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            hash ^= (value >> (8 * byte)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    word(static_cast<std::uint32_t>(mesh.vertices.size()));
    word(static_cast<std::uint32_t>(mesh.indices.size()));
    word(static_cast<std::uint32_t>(mesh.submeshes.size()));
    for (const auto& v : mesh.vertices) {
        word(std::bit_cast<std::uint32_t>(v.px));
        word(std::bit_cast<std::uint32_t>(v.py));
        word(std::bit_cast<std::uint32_t>(v.pz));
    }
    for (auto index : mesh.indices)
        word(index);
    for (const auto& range : mesh.submeshes) {
        word(range.first_index);
        word(range.index_count);
        word(range.material_slot);
    }
    return hash;
}

void remove_empty_ranges(render::CpuMesh& mesh) {
    mesh.submeshes.erase(
        std::remove_if(mesh.submeshes.begin(),
                       mesh.submeshes.end(),
                       [](const render::SubmeshRange& range) { return range.index_count == 0; }),
        mesh.submeshes.end());
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

// MEASURE the reach the real solver enforces, rather than re-deriving it from bone lengths.
// PlayArmIk clamps the shoulder-to-wrist distance into [|upper - lower|, upper + lower] and
// reports the residual as wrist_position_error. So one solve with each wrist pushed 2 m past its
// bind position along the shoulder ray reads the far limit as distance - error, and one with the
// wrist 0.1 mm from the shoulder reads the near limit as distance + error. Both are the solver's
// own arithmetic, so a later bone or clamp change cannot leave a stale copy here. The far solve
// MUST clamp both sides, or the reading is not a limit at all.
bool measure_reach(PlayArmIk& arms,
                   const std::array<core::Vec3, 2>& poles,
                   std::array<float, 2>& near_limit,
                   std::array<float, 2>& far_limit) {
    const auto shoulders = arms.bind_shoulders();
    const auto& wrists = arms.bind_wrists();
    const auto probe = [&](bool far, std::array<float, 2>& out) {
        std::array<core::Mat4, 2> targets{};
        std::array<float, 2> distance{};
        for (unsigned side = 0; side < 2; ++side) {
            const core::Vec3 wrist = core::transform_point(wrists[side], {});
            const core::Vec3 ray = wrist - shoulders[side];
            const float bind = core::length(ray);
            if (!(bind > 1e-4f))
                return false;
            distance[side] = far ? bind + 2.0f : 1e-4f;
            const core::Vec3 target = shoulders[side] + ray * (distance[side] / bind);
            targets[side] = core::mat4_translation(target - wrist) * wrists[side];
        }
        (void)arms.solve(core::Mat4{}, targets, poles);
        const auto& st = arms.stats();
        if (!st.valid_input || (far && st.clamped_targets != 2))
            return false;
        for (unsigned side = 0; side < 2; ++side)
            out[side] = far ? distance[side] - st.wrist_position_error[side]
                            : distance[side] + st.wrist_position_error[side];
        return true;
    };
    if (!probe(true, far_limit) || !probe(false, near_limit))
        return false;
    for (unsigned side = 0; side < 2; ++side)
        if (!(far_limit[side] > near_limit[side] + 4.0f * PlayViewmodel::kReachMargin))
            return false;
    return true;
}

// The largest fraction f in [0, 1] of an action's hand offset whose wrist target stays inside
// the measured reach, kReachMargin in from each limit. The path is the AUTHORED one -- the hand
// still travels toward the magazine or bolt along the same weapon-local line, attached to the
// moving gun -- it just stops where the arm runs out. Projecting the target onto the reach
// sphere instead would be the solver's clamp done one step earlier: the counter would read
// zero while the hand left the path it was asked to follow.
//
// Distance from the shoulder is convex along a straight line, so the reachable part of the
// path that contains f = 0 is one interval and bisection finds its end. If even f = 0 (the
// plain grip on the moved gun) is out of reach nothing here can help: the authored offset is
// kept and the solver's clamp counter reports the frame honestly.
float reachable_fraction(const core::Mat4& arms_from_weapon,
                         core::Vec3 grip,
                         core::Vec3 offset,
                         core::Vec3 shoulder,
                         float near_limit,
                         float far_limit) {
    const auto inside = [&](float f) {
        const float d =
            core::length(core::transform_point(arms_from_weapon, grip + offset * f) - shoulder);
        return d >= near_limit + PlayViewmodel::kReachMargin &&
               d <= far_limit - PlayViewmodel::kReachMargin;
    };
    if (inside(1.0f) || !inside(0.0f))
        return 1.0f;
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20; ++i) { // 1e-6 of the offset: far below a visible millimetre
        const float mid = 0.5f * (lo + hi);
        (inside(mid) ? lo : hi) = mid;
    }
    return lo;
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
        const std::uint32_t original_triangles =
            static_cast<std::uint32_t>(geometry.indices.size() / 3);
        const std::uint64_t fingerprint = i < 3 ? geometry_fingerprint(geometry) : 0;
        MagazineSplit magazine;
        if (i < 3) {
            if (!split_magazine(geometry, static_cast<unsigned>(i), magazine)) {
                std::fprintf(
                    stderr,
                    "99-the-block: %s magazine topology changed; expected authored split\n",
                    kSources[i]);
                return false;
            }
            models_[i].magazine_contact = magazine.contact;
            models_[i].magazine_triangles = magazine.triangles;
        }
        if (i < 3) {
            std::fprintf(
                stderr,
                "99-the-block: %s topology original=%u body=%zu magazine=%u fnv64=%016llx\n",
                kSources[i],
                original_triangles,
                geometry.indices.size() / 3,
                magazine.triangles,
                static_cast<unsigned long long>(fingerprint));
            if (original_triangles != geometry.indices.size() / 3 + magazine.triangles)
                return false;
        }
        // Keep the authored range table for source material lookup. MeshRegistry intentionally
        // discards zero-count ranges; filtering only the upload copies preserves source indices.
        const auto authored_ranges = geometry.submeshes;
        const auto magazine_ranges = magazine.magazine.submeshes;
        remove_empty_ranges(geometry);
        remove_empty_ranges(magazine.magazine);
        const render::MeshId mesh_id = meshes.add(geometry, kSources[i]);
        if (!meshes.contains(mesh_id))
            return false;
        ++resident_meshes_;
        render::MeshId magazine_id = render::kInvalidMeshId;
        if (i < 3) {
            magazine_id = meshes.add(magazine.magazine, "held detachable magazine");
            if (!meshes.contains(magazine_id))
                return false;
            ++resident_meshes_;
        }
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
            if (!measure_reach(model.arms, model.elbow_poles, model.reach_near, model.reach_far)) {
                std::fprintf(
                    stderr, "99-the-block: arm rig %s has no measurable reach\n", kSources[i]);
                return false;
            }
            model.previous.resize(mesh->vertex_count);
            model.last.resize(mesh->vertex_count);
        }
        if (authored_ranges.size() != mesh->submeshes.size())
            return false;
        int normal_mapped = 0;
        int metal_rough_mapped = 0;
        for (std::size_t j = 0; j < authored_ranges.size(); ++j) {
            const auto& range = authored_ranges[j];
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
            if (range.index_count != 0)
                models_[i % 3].parts.push_back({mesh_id,
                                                render::kInvalidMaterialId,
                                                range.first_index,
                                                range.index_count,
                                                i >= 3,
                                                false,
                                                static_cast<std::uint8_t>(i),
                                                static_cast<std::uint32_t>(j)});
            if (i < 3) {
                const auto& mag_range = magazine_ranges[j];
                if (mag_range.index_count != 0)
                    models_[i].parts.push_back({magazine_id,
                                                render::kInvalidMaterialId,
                                                mag_range.first_index,
                                                mag_range.index_count,
                                                false,
                                                true,
                                                static_cast<std::uint8_t>(i),
                                                static_cast<std::uint32_t>(j)});
            }
        }
        // The CC-BY MSR is a solid-material model; retain those source materials
        // honestly. The other weapons and every arm still require their native maps.
        if (i != 2 && (normal_mapped == 0 || metal_rough_mapped == 0)) {
            std::fprintf(
                stderr, "99-the-block: %s needs real normal and metal-rough maps\n", kSources[i]);
            return false;
        }
        if (i >= 3 && models_[i - 3].parts.size() > kMaxDraws - 1) {
            std::fprintf(
                stderr, "99-the-block: %s exceeds first-person draw budget\n", kSources[i]);
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
            const auto material_id = materials.add(desc);
            for (auto& part : models_[i % 3].parts)
                if (part.source_file == i && part.source_range == j)
                    part.material = material_id;
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
    const bool pending_bolt = motion_.pending_bolt();
    motion_.advance(frame_dt, static_cast<PlayWeaponMotion::Weapon>(selected_), fired, aiming_);
    const float speed =
        std::sqrt(actual_velocity.x * actual_velocity.x + actual_velocity.z * actual_velocity.z);
    gait_phase_ += std::min(speed, 10.0f) * dt * 1.6f;
    bob_amount_ = std::clamp(speed / 6.0f, 0.0f, 1.0f);
    if ((fired && !motion_.pending_bolt()) || (pending_bolt && motion_.cycling()))
        recoil_ = std::min(recoil_ + (scoped() ? 0.028f : 0.012f), 0.06f);
    recoil_ *= std::exp(-14.0f * dt);
    const float target_aim = aiming_ && !motion_.reloading() && !motion_.cycling() ? 1.0f : 0.0f;
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
    // Bring the magazine action upward and left in the viewport while preserving the
    // reachable gun depth. Moving the whole gun forward extended both base wrists past
    // their measured limit, where shortening the support-hand offset cannot help.
    local.translation = local.translation + core::Vec3{-0.16f * motion_pose.lower,
                                                       0.14f * motion_pose.lower,
                                                       0.04f * motion_pose.lower};
    local.rotation =
        core::quat_from_euler(recoil_ * .8f + 0.07f * motion_pose.lower,
                              .008f * std::sin(gait_phase_ * .5f) * bob_amount_ * pose_amount,
                              0.24f * motion_pose.tilt);
    const core::Mat4 camera_matrix = core::to_matrix(camera);
    const core::Mat4 weapon_local = core::to_matrix(local) * sight_alignment(index, aim_);
    weapon_local_ = weapon_local;
    const core::Mat4 model = camera_matrix * weapon_local;
    auto& selected = models_[static_cast<unsigned>(selected_)];
    auto grips = selected.grips;
    // Slot zero is the support left hand; slot one is the firing right hand. Moving
    // wrist targets leaves the shoulder anchors fixed and uses the existing CPU IK mesh.
    // Target the measured magazine body, then translate the wrist and detached mesh together.
    // Approach reach and pull reach are solved separately. Once the wrist reaches the socket,
    // any pull limit scales BOTH wrist and mesh displacement; otherwise the magazine stays seated
    // and magazine_contact_gap reports the missed contact on all three authored guns.
    const core::Vec3 pull{0.0f, -0.11f, 0.035f};
    const core::Vec3 support_grip = core::transform_point(grips[0], {});
    const core::Vec3 approach =
        selected.magazine_triangles != 0 ? selected.magazine_contact - support_grip : core::Vec3{};
    const float pull_amount = selected.magazine_triangles != 0 ? motion_pose.magazine_pull : 0.0f;
    const core::Vec3 right_offset{0.025f * motion_pose.right_bolt,
                                  0.035f * motion_pose.right_bolt,
                                  -0.11f * motion_pose.right_bolt + 0.095f * motion_pose.bolt_pull};
    const core::Mat4 arms_from_weapon = selected.arms_from_camera * weapon_local;
    const auto shoulders = selected.arms.bind_shoulders();
    const core::Vec3 approach_path = approach * motion_pose.left_magazine;
    const float approach_fraction = reachable_fraction(arms_from_weapon,
                                                       support_grip,
                                                       approach_path,
                                                       shoulders[0],
                                                       selected.reach_near[0],
                                                       selected.reach_far[0]);
    const core::Vec3 approached = approach_path * approach_fraction;
    if (selected.magazine_triangles != 0 && motion_pose.left_magazine >= .999f &&
        approach_fraction < 1.0f)
        ++reach_stats_.missed_magazine_contacts;
    const float pull_fraction = approach_fraction == 1.0f
                                    ? reachable_fraction(arms_from_weapon,
                                                         support_grip + approached,
                                                         pull * pull_amount,
                                                         shoulders[0],
                                                         selected.reach_near[0],
                                                         selected.reach_far[0])
                                    : 0.0f;
    const core::Vec3 support_offset = approached + pull * (pull_amount * pull_fraction);
    const core::Vec3 authored_support = approach_path + pull * pull_amount;
    reach_stats_.retained[0] =
        core::length(authored_support) > 0.0f
            ? std::min(1.0f, core::length(support_offset) / core::length(authored_support))
            : 1.0f;
    reach_stats_.applied_offset[0] = core::length(support_offset);
    grips[0] = core::mat4_translation(support_offset) * grips[0];
    wrist_world_[0] = core::transform_point(model * grips[0], {});
    const float right_fraction = reachable_fraction(arms_from_weapon,
                                                    core::transform_point(grips[1], {}),
                                                    right_offset,
                                                    shoulders[1],
                                                    selected.reach_near[1],
                                                    selected.reach_far[1]);
    reach_stats_.retained[1] = right_fraction;
    reach_stats_.applied_offset[1] = core::length(right_offset) * right_fraction;
    grips[1] = core::mat4_translation(right_offset * right_fraction) * grips[1];
    wrist_world_[1] = core::transform_point(model * grips[1], {});
    const core::Mat4 magazine_local = core::mat4_translation(pull * (pull_amount * pull_fraction));
    magazine_world_ = model * magazine_local;
    magazine_contact_world_ = core::transform_point(magazine_world_, selected.magazine_contact);
    magazine_contact_gap_ = selected.magazine_triangles != 0
                                ? core::length(wrist_world_[0] - magazine_contact_world_)
                                : 0.0f;
    reach_stats_.limited_poses +=
        approach_fraction < 1.0f || pull_fraction < 1.0f || right_fraction < 1.0f ? 1u : 0u;
    const auto vertices = selected.arms.solve(arms_from_weapon, grips, selected.elbow_poles);
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
        draw.model = part.arm ? camera_matrix * selected.camera_from_arms
                              : (part.magazine ? magazine_world_ : model);
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
