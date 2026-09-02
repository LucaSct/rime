// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Scene extraction + the SceneRenderer (M5.6, ADR-0022). Extraction is a plain function so the
// tests can pin the conventions (camera/light orientation, first-active-camera, draw filtering)
// without a GPU; the renderer wraps it with the uniform uploads and pass declarations.

#include "rime/render/scene_renderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "rime/core/diagnostics/log.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"
#include "rime/render/culling.hpp"

namespace rime::render {

namespace {

// Which pose to DRAW an entity at: its presentation override when something produced one this
// frame, otherwise the simulated pose. See ecs/render_transform.hpp for the contract; the short
// version is that a fixed tick and a free-running frame do not line up, so a producer of smoothed
// motion (m11.6's network interpolation is the first) deposits a per-frame blend that the sim
// itself must never see.
//
// Costs an entity that never blends one null check: RenderTransform is absent on essentially
// everything, and World::get returns null outright for a component that was never even registered —
// which is every world that does not run a client replicator.
[[nodiscard]] const core::Transform& pose_to_draw(const ecs::World& world,
                                                  ecs::Entity e,
                                                  const ecs::WorldTransform& simulated) noexcept {
    if (const auto* presented = world.get<ecs::RenderTransform>(e)) {
        return presented->value;
    }
    return simulated.value;
}

} // namespace

ResolveDrawStats resolve_draws(ExtractedScene& scene, const MeshRegistry& meshes) {
    ResolveDrawStats stats;

    // Built into fresh vectors rather than compacted in place: expansion can GROW the list, so the
    // read-ahead-of-write trick the drop-only version used no longer holds.
    std::vector<DrawItem> out_draws;
    std::vector<ecs::Entity> out_entities;
    out_draws.reserve(scene.draws.size());
    out_entities.reserve(scene.draw_entities.size());

    for (std::size_t i = 0; i < scene.draws.size(); ++i) {
        const DrawItem& src = scene.draws[i];
        if (!meshes.contains(src.mesh)) {
            ++stats.dropped;
            continue;
        }
        const GpuMesh& gpu = meshes.get(src.mesh);
        // Guaranteed non-empty by MeshRegistry::add, which synthesises a whole-mesh range when a
        // CpuMesh carries no table — so this loop needs no "does it have submeshes" branch, and
        // the single-submesh case costs exactly one iteration.
        for (std::size_t s = 0; s < gpu.submeshes.size(); ++s) {
            DrawItem item = src;
            item.first_index = gpu.submeshes[s].first_index;
            item.index_count = gpu.submeshes[s].index_count;
            // The per-slot MATERIAL is not resolved yet: until m16.3 teaches the asset bridge to
            // map `material_slot` through the manifest, every submesh of an entity draws with that
            // entity's single MaterialRef. Splitting the geometry first is what makes that brick a
            // material change rather than a draw-path change as well.
            out_draws.push_back(item);
            out_entities.push_back(scene.draw_entities[i]);
            if (s > 0) {
                ++stats.expanded;
            }
        }
    }

    scene.draws = std::move(out_draws);
    scene.draw_entities = std::move(out_entities);
    return stats;
}

ExtractedScene extract_scene(ecs::World& world) {
    ExtractedScene scene;

    // Draws: every entity wearing the full render wardrobe. Serial for_each is v0 — the loop is
    // a natural par_for_each + per-thread buckets when extraction ever shows up in a profile.
    // The entity rides along in a parallel array so the pick pass can map "the id rasterized at
    // this pixel" back to a live handle (see ExtractedScene::draw_entities).
    world.query<ecs::WorldTransform, MeshRef, MaterialRef>().for_each(
        [&](ecs::Entity e, ecs::WorldTransform& wt, MeshRef& mesh, MaterialRef& mat) {
            if (mesh.mesh == kInvalidMeshId || mat.material == kInvalidMaterialId)
                return; // half-dressed entity: nothing sensible to draw
            scene.draws.push_back(
                {mesh.mesh, mat.material, core::to_matrix(pose_to_draw(world, e, wt))});
            scene.draw_entities.push_back(e);
        });

    // The FIRST active camera wins (the documented rule — deterministic because query order is
    // archetype order, and good enough until a real multi-view story exists). A camera looks
    // down its entity's local −z, so its view matrix is just the inverse of its world matrix.
    world.query<ecs::WorldTransform, Camera>().for_each(
        [&](ecs::Entity e, ecs::WorldTransform& wt, Camera& cam) {
            if (scene.camera.found || !cam.active)
                return;
            const core::Transform& pose = pose_to_draw(world, e, wt);
            scene.camera.found = true;
            scene.camera.view = core::inverse(core::to_matrix(pose));
            scene.camera.position[0] = pose.translation.x;
            scene.camera.position[1] = pose.translation.y;
            scene.camera.position[2] = pose.translation.z;
            scene.camera.fov_y = cam.fov_y;
            scene.camera.z_near = cam.z_near;
            scene.camera.z_far = cam.z_far;
        });

    // Lights, already GPU-shaped. A directional light travels along its entity's −z (the camera
    // convention — aim a light exactly like a camera); transform_vector then normalize keeps it
    // unit under any (positive) scale. Radiance = color × intensity, folded here so the shader
    // never multiplies.
    world.query<ecs::WorldTransform, DirectionalLight>().for_each(
        [&](ecs::Entity e, ecs::WorldTransform& wt, DirectionalLight& l) {
            GpuDirectionalLight g{};
            const core::Vec3 dir = core::normalize(
                core::transform_vector(pose_to_draw(world, e, wt), {0.0f, 0.0f, -1.0f}));
            g.direction[0] = dir.x;
            g.direction[1] = dir.y;
            g.direction[2] = dir.z;
            g.radiance[0] = l.color_r * l.intensity;
            g.radiance[1] = l.color_g * l.intensity;
            g.radiance[2] = l.color_b * l.intensity;
            scene.dir_lights.push_back(g);
        });

    world.query<ecs::WorldTransform, PointLight>().for_each(
        [&](ecs::Entity e, ecs::WorldTransform& wt, PointLight& l) {
            GpuPointLight g{};
            const core::Transform& pose = pose_to_draw(world, e, wt);
            g.position[0] = pose.translation.x;
            g.position[1] = pose.translation.y;
            g.position[2] = pose.translation.z;
            g.position[3] = l.radius;
            g.radiance[0] = l.color_r * l.intensity;
            g.radiance[1] = l.color_g * l.intensity;
            g.radiance[2] = l.color_b * l.intensity;
            scene.point_lights.push_back(g);
        });

    // Spot lights (m10.2): a point light with a cone. Position is the entity's world translation;
    // the cone axis is its −z (the DirectionalLight/Camera "aim it like a camera" convention). The
    // cone half-angles are pre-cosined here so the shadow fit + shader never call trig. Radiance =
    // color × intensity, folded once. These carry the CPU shape the shadow fit needs
    // (pos/dir/cone), not a GPU struct — LocalShadowMap turns each into a perspective view_proj +
    // the GPU record.
    world.query<ecs::WorldTransform, SpotLight>().for_each(
        [&](ecs::Entity e, ecs::WorldTransform& wt, SpotLight& l) {
            SpotLightData s{};
            const core::Transform& pose = pose_to_draw(world, e, wt);
            s.position = pose.translation;
            s.direction = core::normalize(core::transform_vector(pose, {0.0f, 0.0f, -1.0f}));
            s.range = l.range;
            // Guard the cone: outer ≥ inner, both in (0, ~90°), so cos_inner ≥ cos_outer and the
            // shadow FOV (2×outer) never degenerates.
            const float outer = std::clamp(l.outer_angle, 0.01f, 1.5533f);
            const float inner = std::clamp(l.inner_angle, 0.0f, outer);
            s.outer_angle = outer;
            s.cos_inner = std::cos(inner);
            s.cos_outer = std::cos(outer);
            s.radiance[0] = l.color_r * l.intensity;
            s.radiance[1] = l.color_g * l.intensity;
            s.radiance[2] = l.color_b * l.intensity;
            scene.spot_lights.push_back(s);
        });

    return scene;
}

SceneRenderer::SceneRenderer(rhi::Device& device,
                             const MeshRegistry& meshes,
                             const MaterialRegistry& materials)
    : device_(device), meshes_(meshes), materials_(materials), depth_prepass_(device),
      forward_(device), tonemap_(device), csm_(device), local_shadows_(device), clustered_(device),
      sdf_clipmap_(device), ddgi_(device), ssr_(device) {
    // Default ring depth: kFramesInFlight (2, private to the Vulkan swapchain) + 1. See
    // set_frames_in_flight for why the default is the safe maximum rather than the headless
    // minimum.
    set_frames_in_flight(2);

    // The white fallback: one white texel that decodes to 1.0. Multiplying by it is the identity,
    // so the shader needs no "has texture?" branch — the classic dummy-texture trick. It serves
    // FOUR slots (base-color, metallic-roughness, occlusion, emissive): 1.0 is the right identity
    // for each (albedo×1, roughness/metallic factor×1, AO 1 = unoccluded, emissive×1), and white
    // reads 1.0 whether the view srgb-decodes or not, so one texel covers both colour and data
    // slots.
    rhi::TextureDesc td{};
    td.extent = {1, 1};
    td.format = rhi::Format::RGBA8Srgb;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "material-fallback-white";
    white_ = device.create_texture(td);
    const std::uint8_t white_px[4] = {255, 255, 255, 255};
    device.write_texture(white_, white_px, sizeof(white_px));

    // The normal-slot fallback: one flat tangent-space normal (128,128,255), which decodes to +Z,
    // so an un-mapped surface keeps its geometric normal. Unorm (linear): a normal map is DATA, so
    // it must NOT be sRGB-decoded on sampling.
    rhi::TextureDesc nd{};
    nd.extent = {1, 1};
    nd.format = rhi::Format::RGBA8Unorm;
    nd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    nd.debug_name = "material-fallback-flat-normal";
    flat_normal_ = device.create_texture(nd);
    const std::uint8_t flat_normal_px[4] = {128, 128, 255, 255};
    device.write_texture(flat_normal_, flat_normal_px, sizeof(flat_normal_px));

    // One sampler for every base-color map: trilinear + a little anisotropy (silently degrades
    // where unsupported), Repeat so tiled floors tile.
    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Linear;
    sd.min_filter = rhi::Filter::Linear;
    sd.mip_filter = rhi::Filter::Linear;
    sd.max_anisotropy = 8.0f;
    sd.address_mode = rhi::AddressMode::Repeat;
    sd.debug_name = "material-sampler";
    material_sampler_ = device.create_sampler(sd);

    // The placeholder depth array bound where a shadow type is absent (m10.2): 1×1 and 2 layers, so
    // it takes a 2-D-ARRAY view that satisfies the shadowed pipeline's sampler2DArrayShadow
    // bindings even in the sun-only or spot-only case. Never rendered into or sampled (the count-0
    // uniform gates it) — it exists purely so every descriptor points at a valid image.
    rhi::TextureDesc dd{};
    dd.extent = {1, 1};
    dd.array_layers = 2;
    dd.format = kDepthFormat;
    dd.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
    dd.debug_name = "shadow-dummy-array";
    dummy_shadow_array_ = device.create_texture(dd);
    // Park it in ShaderRead once. It is never written, so it stays there forever, and the shadow
    // systems' empty_binding imports it at ShaderRead — no per-frame layout-bookkeeping mismatch.
    {
        auto cmd = device.begin_commands();
        cmd->texture_barrier(
            dummy_shadow_array_, rhi::ResourceState::Undefined, rhi::ResourceState::ShaderRead);
        device.submit_blocking(*cmd);
    }
}

SceneRenderer::~SceneRenderer() {
    device_.destroy(dummy_shadow_array_);
    device_.destroy(material_sampler_);
    device_.destroy(flat_normal_);
    device_.destroy(white_);
    for (rhi::BufferHandle b : draw_ubos_) {
        if (b.is_valid())
            device_.destroy(b);
    }
    for (rhi::BufferHandle b : frame_ubos_) {
        if (b.is_valid())
            device_.destroy(b);
    }
}

void SceneRenderer::set_frames_in_flight(std::uint32_t frames) {
    const std::uint32_t slots = frames + 1; // the swapchain.hpp rule, applied to uniforms
    if (slots == frame_ubos_.size())
        return;

    // Rebuilding the ring destroys buffers, so it is only legal while nothing is in flight — at
    // construction, or before the first frame. Callers are told to call this once, before render().
    for (rhi::BufferHandle b : draw_ubos_) {
        if (b.is_valid())
            device_.destroy(b);
    }
    for (rhi::BufferHandle b : frame_ubos_) {
        if (b.is_valid())
            device_.destroy(b);
    }

    frame_ubos_.clear();
    draw_ubos_.clear();
    draw_capacities_.assign(slots, 0);
    ubo_slot_ = 0;

    rhi::BufferDesc fd{};
    fd.size = sizeof(GpuFrameUniforms);
    fd.usage = rhi::BufferUsage::Uniform;
    fd.memory = rhi::MemoryUsage::CpuToGpu;
    fd.debug_name = "scene-frame-ubo";
    for (std::uint32_t i = 0; i < slots; ++i) {
        frame_ubos_.push_back(device_.create_buffer(fd));
        draw_ubos_.push_back({}); // allocated lazily by ensure_draw_capacity, per slot
    }
}

void SceneRenderer::ensure_draw_capacity(std::uint32_t draw_count) {
    // Grows only THIS FRAME'S SLOT. That is what makes destroy-and-recreate safe again: the slot is
    // one the ring has come back round to, so its previous submission has already been waited on by
    // `acquire_next_image`'s fence. Growing every slot here would be the original bug wearing a
    // ring — it would free buffers the other in-flight frames still have bound.
    const std::size_t s = ubo_slot_;
    if (draw_count <= draw_capacities_[s] && draw_ubos_[s].is_valid())
        return;

    // Grow geometrically from a floor of 64. Slots grow independently, so a scene that settles at a
    // given draw count pays each slot's growth once and then never again.
    std::uint32_t capacity = draw_capacities_[s] == 0 ? 64u : draw_capacities_[s];
    while (capacity < draw_count)
        capacity *= 2;
    if (draw_ubos_[s].is_valid())
        device_.destroy(draw_ubos_[s]);
    rhi::BufferDesc bd{};
    bd.size = static_cast<std::uint64_t>(capacity) * kDrawUniformStride;
    bd.usage = rhi::BufferUsage::Uniform;
    bd.memory = rhi::MemoryUsage::CpuToGpu;
    bd.debug_name = "scene-draw-ubo";
    draw_ubos_[s] = device_.create_buffer(bd);
    draw_capacities_[s] = capacity;
}

void SceneRenderer::sync_sdf_instances(ecs::World& world) {
    // Pass 1 (cheap, full walk): who currently carries a live SdfRef? This component is worn only
    // by GI-relevant geometry (sparse), so walking every match every frame just to notice a
    // despawn/un-ref costs little — the EXPENSIVE half (the GPU-texture-recreating
    // update_instance call) is what pass 2 change-detection-gates.
    std::unordered_set<std::uint64_t> current_keys;
    current_keys.reserve(tracked_sdf_entities_.size());
    world.query<ecs::WorldTransform, SdfRef>().for_each(
        [&](ecs::Entity e, ecs::WorldTransform&, SdfRef& ref) {
            if (ref.source == kInvalidSdfSourceId)
                return; // not registered yet — nothing to feed the clipmap
            current_keys.insert(std::bit_cast<std::uint64_t>(e));
        });
    for (std::uint64_t key : tracked_sdf_entities_) {
        if (current_keys.find(key) == current_keys.end())
            sdf_clipmap_.remove_instance(key); // despawned, or its SdfRef went away/invalid
    }
    tracked_sdf_entities_ = std::move(current_keys);

    // Pass 2 (the C1 seam, ADR-0032): only entities whose WorldTransform or SdfRef actually
    // changed since the last call re-upload — a settled scene costs nothing after its first frame,
    // exactly the discipline SdfClipmap::update_instance's own doc comment asks its caller for.
    world.query<ecs::WorldTransform, SdfRef>().for_each_changed(
        sdf_instances_since_, [&](ecs::Entity e, ecs::WorldTransform& wt, SdfRef& ref) {
            if (ref.source == kInvalidSdfSourceId || ref.source >= sdf_sources_.size())
                return;
            sdf_clipmap_.update_instance(std::bit_cast<std::uint64_t>(e),
                                         sdf_sources_[ref.source],
                                         core::to_matrix(wt.value));
        });
    sdf_instances_since_ = world.version();
}

SceneRenderer::Output SceneRenderer::render(RenderGraph& graph,
                                            ecs::World& world,
                                            rhi::Extent2D extent,
                                            bool use_depth_prepass) {
    // Advance the uniform ring first, so every buffer this frame touches belongs to a slot the
    // presentation fence has already waited on. Done unconditionally — an early-return frame
    // consuming a slot costs nothing and keeps the slot sequence independent of scene content.
    ubo_slot_ = (ubo_slot_ + 1) % static_cast<std::uint32_t>(frame_ubos_.size());

    ExtractedScene scene = extract_scene(world);
    // Before ANY registry lookup: a MeshRef can name a mesh this registry does not hold (a scene
    // file is untrusted input), and every path below — the cull loop, record_draws for the depth,
    // forward and shadow passes — indexes with it.
    const ResolveDrawStats resolved = resolve_draws(scene, meshes_);
    const std::size_t unresolvable = resolved.dropped;
    if (unresolvable != 0) {
        unresolvable_draws_ += unresolvable;
        if (!warned_unresolvable_mesh_) {
            RIME_WARN("render: dropped {} draw(s) naming a mesh id this registry does not hold "
                      "(unresolvable MeshRef — stale or hand-edited scene?)",
                      unresolvable);
            warned_unresolvable_mesh_ = true;
        }
    }
    if (!scene.camera.found) {
        if (!warned_no_camera_) {
            RIME_WARN("render: no active camera in the world — declaring no passes");
            warned_no_camera_ = true;
        }
        return {};
    }

    // ── Frame uniforms ────────────────────────────────────────────────────────────────────
    GpuFrameUniforms fu{};
    const float aspect = extent.height > 0
                             ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                             : 1.0f;
    fu.view_proj =
        core::perspective(scene.camera.fov_y, aspect, scene.camera.z_near, scene.camera.z_far) *
        scene.camera.view;
    // ── View-frustum culling (m13.2a, ADR-0035 §2a) ──────────────────────────────────────
    //
    // Done HERE rather than inside extract_scene because the frustum needs the frame's aspect
    // ratio — which is a property of the target, not of the world — and because the per-mesh
    // bounds live in the registry, which extraction deliberately does not know about.
    //
    // The draws and their parallel entity array are compacted TOGETHER. They are parallel by
    // contract (`ExtractedScene::draw_entities`), and the pick pass reads the pairing to answer
    // "which entity is this pixel"; compacting one and not the other would mis-identify every
    // entity after the first culled draw — a bug with no wrong pixel anywhere.
    // IT PARTITIONS, IT DOES NOT DELETE — and that distinction is the whole of this brick's one
    // real hazard.
    //
    // ONE draw list feeds the camera passes AND both shadow passes (the cascade fit and the local
    // spots render the same geometry from the LIGHT's point of view). Culling it against the
    // CAMERA's frustum therefore removes shadow casters that are off-screen but cast into the
    // view — a ceiling above the camera stops shadowing the floor under it, and the room it was
    // darkening lights up.
    //
    // That is not a hypothetical. The first version of this deleted the culled entries, and
    // `gi_thesis_test` — a covered room lit only by bounce — went from 0.04 to 0.74 on its floor
    // pixel, because its ceiling and divider were no longer drawn into the shadow map. Every other
    // render proof stayed green.
    //
    // So the culled draws are moved to the BACK rather than dropped, and the visible ones keep
    // their relative order at the front. `record_draws` indexes each draw's uniform slice and its
    // five material textures by LOOP POSITION, so every parallel array stays aligned by
    // construction: the camera passes take the first `visible` entries, the shadow passes take all
    // of them, and neither needs to know the other exists.
    std::size_t visible = scene.draws.size();
    if (cull_enabled_) {
        const Frustum frustum = frustum_from_view_proj(fu.view_proj);
        std::size_t front = 0;
        std::vector<DrawItem> hidden_draws;
        std::vector<ecs::Entity> hidden_entities;
        for (std::size_t read = 0; read < scene.draws.size(); ++read) {
            const GpuMesh& mesh = meshes_.get(scene.draws[read].mesh);
            core::Vec3 world_min;
            core::Vec3 world_max;
            transform_aabb(
                scene.draws[read].model, mesh.local_min, mesh.local_max, world_min, world_max);
            if (aabb_in_frustum(frustum, world_min, world_max)) {
                scene.draws[front] = scene.draws[read];
                scene.draw_entities[front] = scene.draw_entities[read];
                ++front;
            } else {
                hidden_draws.push_back(scene.draws[read]);
                hidden_entities.push_back(scene.draw_entities[read]);
            }
        }
        visible = front;
        for (std::size_t i = 0; i < hidden_draws.size(); ++i) {
            scene.draws[front] = hidden_draws[i];
            scene.draw_entities[front] = hidden_entities[i];
            ++front;
        }
        cull_stats_.culled += hidden_draws.size();
        cull_stats_.submitted += visible;
    } else {
        cull_stats_.submitted += scene.draws.size();
    }

    fu.camera_pos[0] = scene.camera.position[0];
    fu.camera_pos[1] = scene.camera.position[1];
    fu.camera_pos[2] = scene.camera.position[2];
    fu.ambient[0] = ambient_[0];
    fu.ambient[1] = ambient_[1];
    fu.ambient[2] = ambient_[2];

    const auto ndir = static_cast<std::uint32_t>(
        std::min<std::size_t>(scene.dir_lights.size(), kMaxDirectionalLights));
    const auto npoint = static_cast<std::uint32_t>(
        std::min<std::size_t>(scene.point_lights.size(), kMaxPointLights));
    // The point-light cap only bites on the unclustered path — with m10.3 on, every point light
    // reaches the shader through the froxel lists and the uniform block is ignored.
    const bool point_overflow = !lighting_.clustered_enabled && scene.point_lights.size() > npoint;
    if ((scene.dir_lights.size() > ndir || point_overflow) && !warned_lights_) {
        RIME_WARN("render: scene exceeds the light caps ({} dir / {} point) — extra lights are "
                  "dropped until per-view light culling exists (M10)",
                  kMaxDirectionalLights,
                  kMaxPointLights);
        warned_lights_ = true;
    }
    for (std::uint32_t i = 0; i < ndir; ++i)
        fu.dir_lights[i] = scene.dir_lights[i];
    for (std::uint32_t i = 0; i < npoint; ++i)
        fu.point_lights[i] = scene.point_lights[i];
    fu.light_counts[0] = ndir;
    fu.light_counts[1] = npoint;
    device_.write_buffer(frame_ubos_[ubo_slot_], &fu, sizeof(fu));

    // ── Per-draw uniforms + resolved textures ─────────────────────────────────────────────
    frame_draws_ = std::move(scene.draws);
    const auto draw_count = static_cast<std::uint32_t>(frame_draws_.size());
    ensure_draw_capacity(std::max(draw_count, 1u));
    draw_staging_.assign(static_cast<std::size_t>(draw_count) * kDrawUniformStride, 0);
    frame_base_color_.resize(draw_count);
    frame_metallic_roughness_.resize(draw_count);
    frame_normal_.resize(draw_count);
    frame_occlusion_.resize(draw_count);
    frame_emissive_.resize(draw_count);
    for (std::uint32_t i = 0; i < draw_count; ++i) {
        const DrawItem& item = frame_draws_[i];
        // Out-of-range material ids are a caller bug, but a defensive default keeps a bad id
        // from becoming an out-of-bounds read.
        const PbrMaterialDesc material =
            item.material < materials_.size() ? materials_.get(item.material) : PbrMaterialDesc{};
        GpuDrawUniforms du{};
        du.model = item.model;
        // Normals transform by the inverse-transpose (see pbr_forward.vert). A degenerate
        // (zero-scale) model has no inverse — fall back to the model matrix rather than feed
        // NaNs to the whole draw.
        const float det = core::determinant(item.model);
        du.normal_matrix =
            std::fabs(det) > 1e-12f ? core::transpose(core::inverse(item.model)) : item.model;
        du.base_color[0] = material.base_color[0];
        du.base_color[1] = material.base_color[1];
        du.base_color[2] = material.base_color[2];
        du.base_color[3] = material.base_color[3];
        du.params[0] = material.metallic;
        du.params[1] = material.roughness;
        du.params[2] = material.normal_scale;
        du.params[3] = material.occlusion_strength;
        du.emissive[0] = material.emissive[0];
        du.emissive[1] = material.emissive[1];
        du.emissive[2] = material.emissive[2];
        du.emissive[3] = material.alpha_cutoff; // 0 = no masking; see PbrMaterialDesc
        std::memcpy(
            &draw_staging_[static_cast<std::size_t>(i) * kDrawUniformStride], &du, sizeof(du));
        // Resolve each slot to its map or the correct fallback, so record_draws never branches on
        // presence: the normal slot falls back to the flat-normal texel, every other slot to white.
        const auto pick = [](rhi::TextureHandle map, rhi::TextureHandle fallback) {
            return map.is_valid() ? map : fallback;
        };
        frame_base_color_[i] = pick(material.base_color_texture, white_);
        frame_metallic_roughness_[i] = pick(material.metallic_roughness_texture, white_);
        frame_normal_[i] = pick(material.normal_texture, flat_normal_);
        frame_occlusion_[i] = pick(material.occlusion_texture, white_);
        frame_emissive_[i] = pick(material.emissive_texture, white_);
    }
    if (draw_count > 0)
        device_.write_buffer(draw_ubos_[ubo_slot_], draw_staging_.data(), draw_staging_.size());

    // The runtime SDF clipmap (m10.4b): a fourth, independent gate. `sync_sdf_instances` (m10.5a)
    // closes the gap this brick's own comment used to name here: every entity carrying
    // (WorldTransform, SdfRef) is now change-detection-fed into the clipmap automatically, so an
    // empty scene (no SdfRef entities) still recomposes cheaply (clears only) whenever the camera
    // crosses a level's own voxel boundary, and settles to zero passes the rest of the time —
    // exactly the ADR-0032 §11 discipline every other M10 technique follows.
    const core::Vec3 camera_pos{
        scene.camera.position[0], scene.camera.position[1], scene.camera.position[2]};
    if (lighting_.sdf_clipmap_enabled) {
        sync_sdf_instances(world);
        sdf_clipmap_.add(graph, camera_pos);
    }

    // DDGI probes (m10.5a trace-and-store, m10.5b consume): a fifth, independent gate, NESTED
    // inside sdf_clipmap_enabled — DDGI sphere-traces the SAME field the block above steps, so it
    // structurally cannot run against a clipmap nobody is updating (settings.hpp's "requires
    // sdf_clipmap_enabled", made a code fact rather than only a documented expectation). Either
    // way, a DdgiBinding always comes out the other side (the real one, or empty_binding's DDGI-off
    // placeholder) — the shadowed pipeline's binding 14/15/16 must be valid regardless of whether
    // DDGI is actually running this frame, exactly the shadow/local/cluster bindings' own
    // discipline.
    const bool has_ddgi = lighting_.sdf_clipmap_enabled && lighting_.ddgi_enabled;
    DdgiBinding ddgi_binding;
    if (has_ddgi) {
        DdgiLightingInputs ddgi_inputs{};
        ddgi_inputs.has_sun = ndir > 0;
        if (ndir > 0) {
            ddgi_inputs.sun_direction = core::Vec3{fu.dir_lights[0].direction[0],
                                                   fu.dir_lights[0].direction[1],
                                                   fu.dir_lights[0].direction[2]};
            ddgi_inputs.sun_radiance[0] = fu.dir_lights[0].radiance[0];
            ddgi_inputs.sun_radiance[1] = fu.dir_lights[0].radiance[1];
            ddgi_inputs.sun_radiance[2] = fu.dir_lights[0].radiance[2];
        }
        ddgi_inputs.sky_radiance[0] = ambient_[0];
        ddgi_inputs.sky_radiance[1] = ambient_[1];
        ddgi_inputs.sky_radiance[2] = ambient_[2];
        ddgi_binding = ddgi_.add(graph, sdf_clipmap_, camera_pos, ddgi_inputs, lighting_);
    } else {
        ddgi_binding = ddgi_.empty_binding(graph);
    }

    // ── Declare the frame ─────────────────────────────────────────────────────────────────
    // The SHADOW view: every draw, because a caster outside the camera's frustum still casts into
    // it. See the partition note above.
    SceneDrawData shadow_data{};
    shadow_data.meshes = &meshes_;
    shadow_data.draws = frame_draws_;
    shadow_data.base_color_textures = frame_base_color_;
    shadow_data.metallic_roughness_textures = frame_metallic_roughness_;
    shadow_data.normal_textures = frame_normal_;
    shadow_data.occlusion_textures = frame_occlusion_;
    shadow_data.emissive_textures = frame_emissive_;
    shadow_data.frame_ubo = frame_ubos_[ubo_slot_];
    shadow_data.draw_ubo = draw_ubos_[ubo_slot_];
    shadow_data.material_sampler = material_sampler_;

    // The CAMERA view: the visible prefix. Every parallel array is sliced to the same length, so
    // `record_draws`'s index-by-loop-position stays correct.
    const std::size_t visible_count = std::min(visible, frame_draws_.size());
    SceneDrawData data{};
    data.meshes = &meshes_;
    data.draws = std::span<const DrawItem>{frame_draws_}.subspan(0, visible_count);
    data.base_color_textures =
        std::span<const rhi::TextureHandle>{frame_base_color_}.subspan(0, visible_count);
    data.metallic_roughness_textures =
        std::span<const rhi::TextureHandle>{frame_metallic_roughness_}.subspan(0, visible_count);
    data.normal_textures =
        std::span<const rhi::TextureHandle>{frame_normal_}.subspan(0, visible_count);
    data.occlusion_textures =
        std::span<const rhi::TextureHandle>{frame_occlusion_}.subspan(0, visible_count);
    data.emissive_textures =
        std::span<const rhi::TextureHandle>{frame_emissive_}.subspan(0, visible_count);
    data.frame_ubo = frame_ubos_[ubo_slot_];
    data.draw_ubo = draw_ubos_[ubo_slot_];
    data.material_sampler = material_sampler_;

    RGTexture depth = graph.create_texture({extent, kDepthFormat, "scene-depth"});
    RGTexture hdr = graph.create_texture({extent, kHdrFormat, "scene-hdr"});
    RGTexture ldr = graph.create_texture({extent, kLdrFormat, "scene-ldr"});
    // SSR G-buffer (m10.7a): allocated ONLY when SSR is on, so the shadowed pass below stays
    // single-attachment and the pre-SSR frame is untouched otherwise. m10.7b's march reads it.
    RGTexture gbuffer;
    if (lighting_.ssr_enabled)
        gbuffer = graph.create_texture({extent, kGbufferFormat, "scene-gbuffer"});
    if (use_depth_prepass)
        depth_prepass_.add(graph, depth, data);
    // The M10 forward path (ADR-0032 §11 regression bridge) runs only when a feature actually has
    // something to do: shadows enabled with a directional light (m10.1 cascades) and/or spot lights
    // (m10.2), clustering enabled with point lights to cull (m10.3), or DDGI actually running
    // (m10.5b) — a scene with DDGI on but no shadows/clusters still needs the shadowed shader,
    // since that is the only pipeline that samples the atlases at all. Otherwise the
    // byte-identical M5.6 forward path.
    const bool has_sun = lighting_.shadows_enabled && ndir > 0;
    // Spot shadows ride the shadowed shader, so they need the shadow gate too (m10.2).
    const bool has_local =
        lighting_.shadows_enabled && lighting_.local_shadows_enabled && !scene.spot_lights.empty();
    const bool has_clusters = lighting_.clustered_enabled && !scene.point_lights.empty();
    // SSR (m10.7a) needs only the G-buffer written, which any draw produces — so it pulls the frame
    // onto the shadowed shader on its own, no light required (an empty scene => empty, valid
    // G-buffer). The march that consumes it is m10.7b.
    const bool has_ssr = lighting_.ssr_enabled;
    if (has_sun || has_local || has_clusters || has_ddgi || has_ssr) {
        // The cascade binding: the real fit when there is a sun, else a valid count-0 placeholder
        // so the shadowed pipeline's binding 7/8 is always satisfied (a spot-only scene).
        ShadowBinding shadow;
        if (has_sun) {
            CascadeInputs ci{};
            ci.camera_view = scene.camera.view;
            ci.fov_y = scene.camera.fov_y;
            ci.aspect = aspect;
            ci.z_near = scene.camera.z_near;
            ci.z_far = scene.camera.z_far;
            ci.light_dir = core::Vec3{fu.dir_lights[0].direction[0],
                                      fu.dir_lights[0].direction[1],
                                      fu.dir_lights[0].direction[2]};
            // shadow_data, not data: a caster off-screen still casts into the view.
            shadow = csm_.add(graph, depth_prepass_, shadow_data, ci, lighting_);
        } else {
            shadow = csm_.empty_binding(graph, dummy_shadow_array_);
        }
        // The local (spot) binding: the cached spot maps, else the same count-0 placeholder.
        const LocalShadowBinding local =
            has_local ? local_shadows_.add(
                            graph, depth_prepass_, shadow_data, scene.spot_lights, lighting_)
                      : local_shadows_.empty_binding(graph, dummy_shadow_array_);
        // The clustered binding (m10.3): the froxel light lists, else the flag-0 placeholder that
        // sends the shader back to the uniform-block light loop.
        ClusterBinding clusters;
        if (has_clusters) {
            ClusterInputs cin{};
            cin.view = scene.camera.view;
            cin.fov_y = scene.camera.fov_y;
            cin.aspect = aspect;
            cin.z_near = scene.camera.z_near;
            cin.z_far = scene.camera.z_far;
            cin.extent = extent;
            clusters = clustered_.add(graph, scene.point_lights, cin);
        } else {
            clusters = clustered_.empty_binding(graph);
        }
        forward_.add_shadowed(graph,
                              hdr,
                              depth,
                              use_depth_prepass,
                              data,
                              shadow,
                              local,
                              clusters,
                              ddgi_binding,
                              gbuffer);
    } else {
        forward_.add(graph, hdr, depth, use_depth_prepass, data);
    }

    // SSR resolve (m10.7b): reflect the frame off itself into a second HDR target the tonemap then
    // reads. Runs only when ssr_enabled (which is also what allocated the G-buffer above), so
    // otherwise the tonemap reads the raw HDR and the frame is byte-identical (ADR-0032 §11).
    RGTexture tonemap_src = hdr;
    if (has_ssr && gbuffer.is_valid()) {
        SsrInputs si{};
        si.view = scene.camera.view;
        si.proj =
            core::perspective(scene.camera.fov_y, aspect, scene.camera.z_near, scene.camera.z_far);
        si.z_near = scene.camera.z_near;
        si.z_far = scene.camera.z_far;
        si.extent = extent;
        si.ambient[0] = ambient_[0];
        si.ambient[1] = ambient_[1];
        si.ambient[2] = ambient_[2];
        si.max_distance = lighting_.ssr_max_distance;
        si.thickness = lighting_.ssr_thickness;
        si.max_steps = lighting_.ssr_max_steps;
        // A second HDR target the resolve draws into (scene_color + reflection); the tonemap reads
        // this instead of the raw HDR.
        const RGTexture hdr_ssr = graph.create_texture({extent, kHdrFormat, "scene-hdr-ssr"});
        // The DDGI binding computed above is the SSR probe fallback (m10.7c): a ray the screen
        // march misses samples this field in its reflection direction instead of returning flat
        // ambient. It is the SAME binding the forward pass consumed (the real atlases with DDGI on,
        // the enabled=0 empty_binding otherwise) — the two passes are just two readers of one
        // field.
        ssr_.add(graph,
                 hdr,
                 gbuffer,
                 depth,
                 hdr_ssr,
                 si,
                 ddgi_binding.irradiance,
                 ddgi_binding.visibility,
                 ddgi_binding.ubo,
                 ddgi_binding.sampler);
        tonemap_src = hdr_ssr;
    }
    tonemap_.add(graph, tonemap_src, ldr);
    graph.export_texture(ldr); // the frame output; hdr is exportable by the caller when needed
    // Report the HDR the tonemap actually consumed as `hdr`: with SSR on that is the resolved,
    // reflection-added target (tonemap_src); with SSR off it is the raw forward HDR, unchanged.
    // This is what a caller wanting the scene's HDR colour should read (and what the GPU proofs
    // assert on, like the DDGI thesis test — never the tonemapped LDR through the pass chain).
    return {tonemap_src, ldr, gbuffer};
}

} // namespace rime::render
