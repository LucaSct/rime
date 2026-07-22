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
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"

namespace rime::render {

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
            scene.draws.push_back({mesh.mesh, mat.material, core::to_matrix(wt.value)});
            scene.draw_entities.push_back(e);
        });

    // The FIRST active camera wins (the documented rule — deterministic because query order is
    // archetype order, and good enough until a real multi-view story exists). A camera looks
    // down its entity's local −z, so its view matrix is just the inverse of its world matrix.
    world.query<ecs::WorldTransform, Camera>().for_each([&](ecs::WorldTransform& wt, Camera& cam) {
        if (scene.camera.found || !cam.active)
            return;
        scene.camera.found = true;
        scene.camera.view = core::inverse(core::to_matrix(wt.value));
        scene.camera.position[0] = wt.value.translation.x;
        scene.camera.position[1] = wt.value.translation.y;
        scene.camera.position[2] = wt.value.translation.z;
        scene.camera.fov_y = cam.fov_y;
        scene.camera.z_near = cam.z_near;
        scene.camera.z_far = cam.z_far;
    });

    // Lights, already GPU-shaped. A directional light travels along its entity's −z (the camera
    // convention — aim a light exactly like a camera); transform_vector then normalize keeps it
    // unit under any (positive) scale. Radiance = color × intensity, folded here so the shader
    // never multiplies.
    world.query<ecs::WorldTransform, DirectionalLight>().for_each(
        [&](ecs::WorldTransform& wt, DirectionalLight& l) {
            GpuDirectionalLight g{};
            const core::Vec3 dir =
                core::normalize(core::transform_vector(wt.value, {0.0f, 0.0f, -1.0f}));
            g.direction[0] = dir.x;
            g.direction[1] = dir.y;
            g.direction[2] = dir.z;
            g.radiance[0] = l.color_r * l.intensity;
            g.radiance[1] = l.color_g * l.intensity;
            g.radiance[2] = l.color_b * l.intensity;
            scene.dir_lights.push_back(g);
        });

    world.query<ecs::WorldTransform, PointLight>().for_each(
        [&](ecs::WorldTransform& wt, PointLight& l) {
            GpuPointLight g{};
            g.position[0] = wt.value.translation.x;
            g.position[1] = wt.value.translation.y;
            g.position[2] = wt.value.translation.z;
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
        [&](ecs::WorldTransform& wt, SpotLight& l) {
            SpotLightData s{};
            s.position = wt.value.translation;
            s.direction = core::normalize(core::transform_vector(wt.value, {0.0f, 0.0f, -1.0f}));
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
      sdf_clipmap_(device), ddgi_(device) {
    rhi::BufferDesc fd{};
    fd.size = sizeof(GpuFrameUniforms);
    fd.usage = rhi::BufferUsage::Uniform;
    fd.memory = rhi::MemoryUsage::CpuToGpu;
    fd.debug_name = "scene-frame-ubo";
    frame_ubo_ = device.create_buffer(fd);

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
    if (draw_ubo_.is_valid())
        device_.destroy(draw_ubo_);
    device_.destroy(frame_ubo_);
}

void SceneRenderer::ensure_draw_capacity(std::uint32_t draw_count) {
    if (draw_count <= draw_capacity_)
        return;
    // Grow geometrically from a floor of 64. Destroy-and-recreate is safe in the v0 blocking
    // model (the GPU is idle between frames); frames-in-flight will demand per-frame buffering
    // here — a documented seam, not an accident.
    std::uint32_t capacity = draw_capacity_ == 0 ? 64u : draw_capacity_;
    while (capacity < draw_count)
        capacity *= 2;
    if (draw_ubo_.is_valid())
        device_.destroy(draw_ubo_);
    rhi::BufferDesc bd{};
    bd.size = static_cast<std::uint64_t>(capacity) * kDrawUniformStride;
    bd.usage = rhi::BufferUsage::Uniform;
    bd.memory = rhi::MemoryUsage::CpuToGpu;
    bd.debug_name = "scene-draw-ubo";
    draw_ubo_ = device_.create_buffer(bd);
    draw_capacity_ = capacity;
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
    ExtractedScene scene = extract_scene(world);
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
    device_.write_buffer(frame_ubo_, &fu, sizeof(fu));

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
        device_.write_buffer(draw_ubo_, draw_staging_.data(), draw_staging_.size());

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
    SceneDrawData data{};
    data.meshes = &meshes_;
    data.draws = frame_draws_;
    data.base_color_textures = frame_base_color_;
    data.metallic_roughness_textures = frame_metallic_roughness_;
    data.normal_textures = frame_normal_;
    data.occlusion_textures = frame_occlusion_;
    data.emissive_textures = frame_emissive_;
    data.frame_ubo = frame_ubo_;
    data.draw_ubo = draw_ubo_;
    data.material_sampler = material_sampler_;

    RGTexture depth = graph.create_texture({extent, kDepthFormat, "scene-depth"});
    RGTexture hdr = graph.create_texture({extent, kHdrFormat, "scene-hdr"});
    RGTexture ldr = graph.create_texture({extent, kLdrFormat, "scene-ldr"});
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
    if (has_sun || has_local || has_clusters || has_ddgi) {
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
            shadow = csm_.add(graph, depth_prepass_, data, ci, lighting_);
        } else {
            shadow = csm_.empty_binding(graph, dummy_shadow_array_);
        }
        // The local (spot) binding: the cached spot maps, else the same count-0 placeholder.
        const LocalShadowBinding local =
            has_local
                ? local_shadows_.add(graph, depth_prepass_, data, scene.spot_lights, lighting_)
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
        forward_.add_shadowed(
            graph, hdr, depth, use_depth_prepass, data, shadow, local, clusters, ddgi_binding);
    } else {
        forward_.add(graph, hdr, depth, use_depth_prepass, data);
    }
    tonemap_.add(graph, hdr, ldr);
    graph.export_texture(ldr); // the frame output; hdr is exportable by the caller when needed
    return {hdr, ldr};
}

} // namespace rime::render
