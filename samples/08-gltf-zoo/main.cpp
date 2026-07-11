// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 08-gltf-zoo — Milestone 6's "done when": import → cook → load → render real glTF models with
// textures, through the whole offline→runtime asset pipeline. A small turntable gallery of the
// hand-authored models in `assets/` (a textured cube, a normal-mapped metallic-roughness sphere,
// and a rigged figure shown in a sampled static pose — the AN0 preview), each cooked to RMA1 by
// `rime-cli cook`, loaded back through `engine/assets`, uploaded via the GPU asset bridge, and
// drawn with PBR + tonemap through the render graph. It closes M6 the way M5 closed with
// 07-first-light: the milestone ends in a runnable proof, not a compile.
//
// What each stage of the pipeline this exercises:
//   • the cook  — `rime cook assets/ --out <dir>` writes one manifest.txt + the .rmesh/.rmat/.rtex
//                 (+ .rskel/.ranim) files. A CTest fixture runs it before the headless self-check.
//   • load      — the manifest is the index: for every cooked mesh we resolve its material (by the
//                 `<source>#materialN` link), and each material's texture ids back to cooked files.
//                 Meshes + textures load ASYNC on the job system (AssetServer, M6.5); materials,
//                 skeletons, and clips are small fixed records read synchronously.
//   • GPU       — the GpuAssetBridge (M6.6 / ADR-0025) uploads a cooked texture's offline mip chain
//                 verbatim and swaps a material's magenta placeholder for the real texture on
//                 drain.
//   • AN0       — a rigged mesh is deformed once on the CPU: sample its clip into a joint palette
//                 (M6.7's sampler) and linear-blend-skin the vertices into a static posed mesh.
//
// Run it:   build/dev/bin/gltf_zoo --headless [--cooked <dir>] [--frames 30] [--ppm out.ppm]
//           build/dev/bin/gltf_zoo --serve [--cooked <dir>] [--host 0.0.0.0] [--port 9100]
//           build/dev/bin/gltf_zoo --serve --slow-io 400   # stagger the texture pop-in for the
//           demo

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/assets/asset_server.hpp"
#include "rime/assets/clip_asset.hpp"
#include "rime/assets/cooked_reader.hpp"
#include "rime/assets/manifest.hpp"
#include "rime/assets/material_asset.hpp"
#include "rime/assets/mesh_asset.hpp"
#include "rime/assets/skeleton_asset.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/system.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/platform/clock.hpp"
#include "rime/platform/filesystem.hpp"
#include "rime/platform/socket.hpp"
#include "rime/render/components.hpp"
#include "rime/render/gpu_asset_bridge.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/frame_streamer.hpp"
#include "rime/stream/protocol.hpp"

namespace {

using namespace rime;

constexpr std::uint32_t kWidth = 1280;
constexpr std::uint32_t kHeight = 720; // 720p — the dev-server → Mac streaming budget

// Where cooked assets live by default: the CTest fixture cooks into this build-tree path, baked in
// by CMake. A `--cooked <dir>` overrides it (a hand cook, or a packaged game's content dir).
#ifndef RIME_ZOO_COOKED_DIR
#define RIME_ZOO_COOKED_DIR "cooked"
#endif

// ── The turntable: a sim component + system ──────────────────────────────────────────────────────
// Each model rests at its slot on the floor and spins slowly about +Y so every side faces the light
// in turn — the classic asset-gallery presentation, and the thing that makes a normal map earn its
// keep (the perturbed highlight sweeps across the surface as it turns). The spin is SIMULATION,
// advanced by the fixed tick (M5.7), so it turns at the same rate regardless of frame rate.
struct Turntable {
    core::Vec3 base{};  // the model's resting translation (its slot on the floor)
    float phase = 0.0f; // current spin angle [rad]
    float speed = 0.6f; // radians per second
};

// The orbiting point light (07-first-light's, verbatim in spirit): a moving highlight so a streamed
// turntable looks alive and the self-check has motion to average over.
struct LightOrbit {
    float phase = 0.0f;
    float radius = 6.0f;
    float height = 5.0f;
    float speed = 0.7f;
};

// The camera as orbit parameters, atomic so the --serve input thread can steer while the frame loop
// reads (independent scalars — the 04-remote-view pattern).
struct OrbitView {
    std::atomic<float> yaw{0.5f};
    std::atomic<float> pitch{0.30f};
    std::atomic<float> distance{9.5f};
};

// ── Camera + I/O helpers (07-first-light's, unchanged) ───────────────────────────────────────────
core::Transform camera_transform(float yaw, float pitch, float distance, core::Vec3 target) {
    const core::Quat q = core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) *
                                         core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -pitch));
    const core::Vec3 back = core::rotate(q, {0.0f, 0.0f, 1.0f});
    core::Transform t{};
    t.rotation = q;
    t.translation = target + back * distance;
    return t;
}

std::vector<std::uint8_t>
read_rgba8(rhi::Device& device, rhi::TextureHandle tex, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 4;
    rhi::BufferDesc bd{};
    bd.size = bytes;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle rb = device.create_buffer(bd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(tex, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

void write_ppm(const char* path,
               const std::vector<std::uint8_t>& px,
               std::uint32_t w,
               std::uint32_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
        std::fwrite(&px[i * 4], 1, 3, f);
    std::fclose(f);
    std::printf("  wrote %s\n", path);
}

// Read a packed little-endian scalar out of the interleaved vertex blob. memcpy, not a pointer
// cast: the blob is bytes, and a strict-aliasing/alignment-clean read keeps UBSan quiet (the cook
// writes exactly these bytes; the schema hash already proved the layout).
float read_f32(const std::byte* p) {
    float v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

std::uint16_t read_u16(const std::byte* p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// The floor checker (07-first-light's): a mipmapped red/white checker uploaded with a full chain,
// sampled trilinear + anisotropic. sRGB so the shader decodes to linear. The mip/aniso path,
// underfoot — the cooked-texture path is what the *models* exercise, above it.
rhi::TextureHandle make_checker(rhi::Device& device) {
    constexpr std::uint32_t kSize = 256;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(kSize) * kSize * 4);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const bool a = ((x / 32) + (y / 32)) % 2 == 0;
            std::uint8_t* p = &px[(static_cast<std::size_t>(y) * kSize + x) * 4];
            p[0] = a ? 200 : 30;
            p[1] = a ? 60 : 34;
            p[2] = a ? 52 : 44;
            p[3] = 255;
        }
    }
    rhi::TextureDesc td{};
    td.extent = {kSize, kSize};
    td.format = rhi::Format::RGBA8Srgb;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.mip_levels = 9;
    td.debug_name = "floor-checker";
    const rhi::TextureHandle t = device.create_texture(td);
    device.write_texture(t, px.data(), px.size());
    return t;
}

// ── Cooked mesh → renderable mesh ────────────────────────────────────────────────────────────────
// The byte offset of one attribute inside the interleaved vertex, given the mesh's attribute set —
// the sum of the sizes of the attributes that precede it in the fixed order (matches the cook's
// `expected_vertex_stride`). Caller checks `has_attrib` first for optional attributes.
std::uint32_t attrib_offset(assets::VertexAttribs set, assets::VertexAttribs which) {
    using assets::attrib_size;
    using assets::has_attrib;
    using A = assets::VertexAttribs;
    std::uint32_t off = 0;
    for (const A bit : {A::Position, A::Normal, A::Uv, A::Tangent, A::Joints, A::Weights}) {
        if (bit == which)
            return off;
        if (has_attrib(set, bit))
            off += attrib_size(bit);
    }
    return off;
}

// Rescale + recenter a mesh so it presents at a common gallery size: uniformly scaled to fit
// `target` on its largest axis, centered on X/Z, and dropped so its base sits on y = 0. Uniform
// scale leaves the (unit) normals valid, so only positions move. Keeps the turntable transform
// trivial — the models arrive in wildly different source units, but on the floor they're siblings.
void normalize_for_display(render::CpuMesh& mesh, float target) {
    if (mesh.vertices.empty())
        return;
    core::Vec3 lo{mesh.vertices[0].px, mesh.vertices[0].py, mesh.vertices[0].pz};
    core::Vec3 hi = lo;
    for (const auto& v : mesh.vertices) {
        lo = {std::min(lo.x, v.px), std::min(lo.y, v.py), std::min(lo.z, v.pz)};
        hi = {std::max(hi.x, v.px), std::max(hi.y, v.py), std::max(hi.z, v.pz)};
    }
    const core::Vec3 size{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
    const float max_dim = std::max({size.x, size.y, size.z, 1e-4f});
    const float s = target / max_dim;
    const float cx = (lo.x + hi.x) * 0.5f;
    const float cz = (lo.z + hi.z) * 0.5f;
    for (auto& v : mesh.vertices) {
        v.px = (v.px - cx) * s;
        v.py = (v.py - lo.y) * s; // base to the floor
        v.pz = (v.pz - cz) * s;
    }
}

// Unpack a cooked MeshAsset's interleaved blob into the renderer's 48-byte tangented vertex. A mesh
// with no cooked tangents (no normal map) gets them derived on the CPU so the single
// always-tangented forward pipeline serves it too (M6.4). Skin attributes, if present, are ignored
// here — the posed path below reads them.
render::CpuMesh to_cpu_mesh(const assets::MeshAsset& m) {
    using A = assets::VertexAttribs;
    render::CpuMesh out;
    out.indices = m.indices;
    out.vertices.resize(m.vertex_count);
    const bool has_n = assets::has_attrib(m.attribs, A::Normal);
    const bool has_uv = assets::has_attrib(m.attribs, A::Uv);
    const bool has_t = assets::has_attrib(m.attribs, A::Tangent);
    const std::uint32_t off_p = attrib_offset(m.attribs, A::Position);
    const std::uint32_t off_n = attrib_offset(m.attribs, A::Normal);
    const std::uint32_t off_uv = attrib_offset(m.attribs, A::Uv);
    const std::uint32_t off_t = attrib_offset(m.attribs, A::Tangent);
    for (std::uint32_t i = 0; i < m.vertex_count; ++i) {
        const std::byte* base = m.vertices.data() + static_cast<std::size_t>(i) * m.vertex_stride;
        render::MeshVertex v;
        v.px = read_f32(base + off_p);
        v.py = read_f32(base + off_p + 4);
        v.pz = read_f32(base + off_p + 8);
        if (has_n) {
            v.nx = read_f32(base + off_n);
            v.ny = read_f32(base + off_n + 4);
            v.nz = read_f32(base + off_n + 8);
        }
        if (has_uv) {
            v.u = read_f32(base + off_uv);
            v.v = read_f32(base + off_uv + 4);
        }
        if (has_t) {
            v.tx = read_f32(base + off_t);
            v.ty = read_f32(base + off_t + 4);
            v.tz = read_f32(base + off_t + 8);
            v.tw = read_f32(base + off_t + 12);
        }
        out.vertices[i] = v;
    }
    if (!has_t && has_uv && has_n)
        render::compute_tangents(out);
    return out;
}

// Deform a skinned cooked mesh into a static posed mesh, CPU-side — the honest AN0 preview until
// GPU palette skinning (AN1). Sample the clip at `t` into the joint palette (M6.7's sampler), then
// linear- blend-skin every vertex:  p' = Σ_k w_k · palette[j_k] · p  (docs/math/skinning.md). Bakes
// the pose into geometry the existing static path renders — no runtime skinning, no shader change.
render::CpuMesh pose_skinned(const assets::MeshAsset& m,
                             const assets::Skeleton& skeleton,
                             const assets::Clip& clip,
                             float t) {
    using A = assets::VertexAttribs;
    std::vector<core::Mat4> palette(skeleton.joint_count(), core::identity());
    // A shape mismatch returns 0 and leaves the palette identity ⇒ the mesh renders in its bind
    // pose (a truthful fallback, not a crash — frame-code discipline).
    (void)assets::sample_clip(clip, skeleton, t, assets::TimePolicy::Clamp, palette);

    render::CpuMesh out;
    out.indices = m.indices;
    out.vertices.resize(m.vertex_count);
    const bool has_n = assets::has_attrib(m.attribs, A::Normal);
    const bool has_uv = assets::has_attrib(m.attribs, A::Uv);
    const bool has_j = assets::has_attrib(m.attribs, A::Joints);
    const bool has_w = assets::has_attrib(m.attribs, A::Weights);
    const std::uint32_t off_p = attrib_offset(m.attribs, A::Position);
    const std::uint32_t off_n = attrib_offset(m.attribs, A::Normal);
    const std::uint32_t off_uv = attrib_offset(m.attribs, A::Uv);
    const std::uint32_t off_j = attrib_offset(m.attribs, A::Joints);
    const std::uint32_t off_w = attrib_offset(m.attribs, A::Weights);
    const std::uint32_t n_joints = static_cast<std::uint32_t>(skeleton.joint_count());
    for (std::uint32_t i = 0; i < m.vertex_count; ++i) {
        const std::byte* base = m.vertices.data() + static_cast<std::size_t>(i) * m.vertex_stride;
        const core::Vec3 bind_p{
            read_f32(base + off_p), read_f32(base + off_p + 4), read_f32(base + off_p + 8)};
        const core::Vec3 bind_n = has_n ? core::Vec3{read_f32(base + off_n),
                                                     read_f32(base + off_n + 4),
                                                     read_f32(base + off_n + 8)}
                                        : core::Vec3{0.0f, 0.0f, 1.0f};
        core::Vec3 p{0.0f, 0.0f, 0.0f};
        core::Vec3 nrm{0.0f, 0.0f, 0.0f};
        float wsum = 0.0f;
        if (has_j && has_w) {
            for (int k = 0; k < 4; ++k) {
                const float w = read_f32(base + off_w + static_cast<std::uint32_t>(k) * 4);
                if (w == 0.0f)
                    continue;
                std::uint16_t j = read_u16(base + off_j + static_cast<std::uint32_t>(k) * 2);
                if (j >= n_joints)
                    j = 0;
                p = p + core::transform_point(palette[j], bind_p) * w;
                nrm = nrm + core::transform_vector(palette[j], bind_n) * w;
                wsum += w;
            }
        }
        render::MeshVertex v;
        if (wsum > 1e-6f) {
            v.px = p.x;
            v.py = p.y;
            v.pz = p.z;
            const core::Vec3 nn = core::normalize(nrm);
            v.nx = nn.x;
            v.ny = nn.y;
            v.nz = nn.z;
        } else { // an unskinned vertex keeps its bind pose
            v.px = bind_p.x;
            v.py = bind_p.y;
            v.pz = bind_p.z;
            v.nx = bind_n.x;
            v.ny = bind_n.y;
            v.nz = bind_n.z;
        }
        if (has_uv) {
            v.u = read_f32(base + off_uv);
            v.v = read_f32(base + off_uv + 4);
        }
        out.vertices[i] = v;
    }
    if (has_uv && has_n)
        render::compute_tangents(out);
    return out;
}

// ── A loaded model and its material's streaming textures ─────────────────────────────────────────
// A material names its textures by content id; the bridge hands out a handle per requested texture.
// We keep the handles (not the resolved rhi textures) so a per-frame refresh can swap the magenta
// placeholder for the real cooked texture the moment the bridge drains it (the M6.5 async story,
// made visible by --slow-io).
struct ModelTextures {
    assets::TextureAssetHandle base_color{};
    assets::TextureAssetHandle metallic_roughness{};
    assets::TextureAssetHandle normal{};
    assets::TextureAssetHandle occlusion{};
    assets::TextureAssetHandle emissive{};
};

// Which maps a control render strips, to isolate one feature's contribution (the headless proof).
struct Strip {
    bool base_color = false;
    bool normal = false;
};

render::PbrMaterialDesc build_desc(const assets::MaterialAsset& m,
                                   const ModelTextures& tex,
                                   const render::GpuAssetBridge& bridge,
                                   Strip strip) {
    render::PbrMaterialDesc d{};
    d.base_color[0] = m.base_color[0];
    d.base_color[1] = m.base_color[1];
    d.base_color[2] = m.base_color[2];
    d.base_color[3] = m.base_color[3];
    d.metallic = m.metallic;
    d.roughness = m.roughness;
    d.emissive[0] = m.emissive[0];
    d.emissive[1] = m.emissive[1];
    d.emissive[2] = m.emissive[2];
    d.normal_scale = m.normal_scale;
    d.occlusion_strength = m.occlusion_strength;
    const auto resolve = [&](assets::TextureAssetHandle h) {
        return h.is_valid() ? bridge.texture_or_placeholder(h) : rhi::TextureHandle{};
    };
    if (!strip.base_color)
        d.base_color_texture = resolve(tex.base_color);
    d.metallic_roughness_texture = resolve(tex.metallic_roughness);
    if (!strip.normal)
        d.normal_texture = resolve(tex.normal);
    d.occlusion_texture = resolve(tex.occlusion);
    d.emissive_texture = resolve(tex.emissive);
    return d;
}

struct LoadedModel {
    std::string name;
    render::MaterialId material = render::kInvalidMaterialId;
    assets::MaterialAsset material_asset;
    ModelTextures textures;
    ecs::Entity entity{};
    bool rigged = false;
    std::uint32_t triangles = 0;
};

// ── The zoo: the pieces every mode shares ────────────────────────────────────────────────────────
struct Zoo {
    app::Application& app; // borrowed — the caller constructs it and CHECKS its device first
    std::filesystem::path cooked_dir;
    render::MeshRegistry meshes;
    render::MaterialRegistry materials;
    render::SceneRenderer renderer;
    assets::AssetServer server; // async loads run on the app's job system
    render::GpuAssetBridge bridge;
    rhi::TextureHandle checker{};
    ecs::Entity camera{};
    std::vector<LoadedModel> models;
    render::RGTexture last_ldr{};

    explicit Zoo(app::Application& application, std::filesystem::path dir)
        : app(application), cooked_dir(std::move(dir)), meshes(*app.device()),
          renderer(*app.device(), meshes, materials), server(app.jobs()),
          bridge(*app.device(), server) {
        using ecs::WorldTransform;
        ecs::World& world = app.world();
        render::register_render_components(world);
        (void)world.register_component<Turntable>();
        (void)world.register_component<LightOrbit>();

        checker = make_checker(*app.device());
        build_stage();
        load_models();

        renderer.set_ambient(0.04f, 0.04f, 0.05f);
        app.on_render([this](app::FrameContext& ctx) {
            last_ldr = renderer.render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
        });
    }

    ~Zoo() { app.device()->destroy(checker); }

    Zoo(const Zoo&) = delete;
    Zoo& operator=(const Zoo&) = delete;

    // The floor, the lights, the camera, and the two sim systems (turntable spin + orbiting light).
    void build_stage() {
        using ecs::WorldTransform;
        ecs::World& world = app.world();

        const render::MeshId floor = meshes.add(render::make_plane(16.0f, 8.0f), "floor");
        render::PbrMaterialDesc floor_mat{};
        floor_mat.metallic = 0.0f;
        floor_mat.roughness = 0.8f;
        floor_mat.base_color_texture = checker;
        core::Transform floor_tf{};
        (void)world.spawn_with(WorldTransform{floor_tf},
                               render::MeshRef{floor},
                               render::MaterialRef{materials.add(floor_mat)});

        camera = world.spawn_with(
            WorldTransform{camera_transform(0.5f, 0.30f, 9.5f, {0.0f, 0.6f, 0.0f})},
            render::Camera{});

        // A dim directional sun (aimed down-forward) plus an orbiting point light: the sun gives
        // the metallic-roughness sweep a stable key, the point light adds a moving highlight so the
        // normal map's perturbation is unmistakable as the models turn.
        core::Transform sun_tf{};
        sun_tf.rotation = core::normalize(core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.05f) *
                                          core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.6f));
        (void)world.spawn_with(WorldTransform{sun_tf},
                               render::DirectionalLight{1.0f, 0.97f, 0.9f, 2.2f});

        core::Transform lt{};
        lt.translation = {6.0f, 5.0f, 0.0f};
        (void)world.spawn_with(
            WorldTransform{lt}, render::PointLight{1.0f, 0.9f, 0.8f, 60.0f, 22.0f}, LightOrbit{});

        const double dt = app.fixed_dt();
        app.schedule().add(
            "turntable",
            ecs::SystemAccess{{}, ecs::signature_of<Turntable, WorldTransform>(world)},
            [dt](ecs::World& w, core::JobSystem&, ecs::CommandBuffer&) {
                w.query<Turntable, WorldTransform>().for_each(
                    [dt](Turntable& t, WorldTransform& wt) {
                        t.phase += static_cast<float>(dt) * t.speed;
                        core::Transform tf{};
                        tf.translation = t.base;
                        tf.rotation = core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, t.phase);
                        wt.value = tf;
                    });
            });
        app.schedule().add(
            "orbit-light",
            ecs::SystemAccess{{}, ecs::signature_of<LightOrbit, WorldTransform>(world)},
            [dt](ecs::World& w, core::JobSystem&, ecs::CommandBuffer&) {
                w.query<LightOrbit, WorldTransform>().for_each(
                    [dt](LightOrbit& o, WorldTransform& wt) {
                        o.phase += static_cast<float>(dt) * o.speed;
                        wt.value.translation = {
                            o.radius * std::cos(o.phase), o.height, o.radius * std::sin(o.phase)};
                    });
            });
    }

    // Discover every cooked model from the manifest and load it: async mesh + textures, synchronous
    // material/skeleton/clip. The manifest is the whole index — the sample reads no glTF, only
    // RMA1.
    void load_models() {
        using ecs::WorldTransform;
        const auto manifest_text = platform::read_file(cooked_dir / "manifest.txt");
        if (!manifest_text) {
            std::fprintf(stderr,
                         "08-gltf-zoo: no manifest at %s — run `rime cook "
                         "samples/08-gltf-zoo/assets --out %s` first\n",
                         (cooked_dir / "manifest.txt").string().c_str(),
                         cooked_dir.string().c_str());
            return;
        }
        const auto manifest = assets::Manifest::parse(std::string_view(
            reinterpret_cast<const char*>(manifest_text->data()), manifest_text->size()));
        if (!manifest) {
            std::fprintf(stderr, "08-gltf-zoo: manifest is malformed\n");
            return;
        }

        // Pass 1: request every cooked mesh (async) and remember its source path, so pass 2 can
        // link materials/skeletons by the `<source>#...` naming the cook emits.
        struct Pending {
            std::string source;
            std::string cooked_file;
            assets::MeshAssetHandle handle;
        };

        std::vector<Pending> pending;
        for (const auto& e : manifest->entries()) {
            if (e.kind == assets::AssetKind::Mesh) {
                pending.push_back({e.source_path,
                                   e.cooked_file,
                                   server.request_mesh(cooked_dir / e.cooked_file)});
            }
        }
        server
            .wait_for_pending_loads(); // block for geometry: a model with no mesh is worse than one
        server.pump();                 // with placeholder textures

        // Pass 2: build each model. Geometry is resident now; materials/skeletons/clips are small
        // fixed records we read synchronously; textures are requested through the bridge (async)
        // and resolve to the magenta placeholder until drained.
        int slot = 0;
        const int count = static_cast<int>(pending.size());
        for (const auto& p : pending) {
            const assets::MeshAsset* mesh = server.get(p.handle);
            if (!mesh) {
                std::fprintf(
                    stderr, "08-gltf-zoo: mesh '%s' failed to load\n", p.cooked_file.c_str());
                continue;
            }
            LoadedModel lm;
            lm.name = std::filesystem::path(p.cooked_file).stem().string();

            // The skeleton + first clip cooked alongside this mesh, if any (`<source>#skeleton`,
            // `<source>#animation/<name>`) — a skinned model is posed once by the CPU sampler.
            render::CpuMesh cpu;
            const assets::ManifestEntry* skel_e = find_source(*manifest, p.source + "#skeleton");
            const assets::ManifestEntry* clip_e = find_prefix(*manifest, p.source + "#animation/");
            if (skel_e && clip_e) {
                auto skel = load_skeleton(cooked_dir / skel_e->cooked_file);
                auto clip = load_clip(cooked_dir / clip_e->cooked_file);
                if (skel && clip) {
                    // Half-duration: a clearly-posed frame, unmistakably different from bind.
                    cpu = pose_skinned(*mesh, *skel, *clip, clip->duration * 0.5f);
                    lm.rigged = true;
                    std::printf("  %s: rigged (%zu joints), posed at t=%.2fs\n",
                                lm.name.c_str(),
                                skel->joint_count(),
                                clip->duration * 0.5f);
                }
            }
            if (cpu.vertices.empty())
                cpu = to_cpu_mesh(*mesh);
            lm.triangles = static_cast<std::uint32_t>(cpu.indices.size() / 3);
            normalize_for_display(cpu, 1.6f);
            const render::MeshId mesh_id = meshes.add(cpu, lm.name);

            // The material for this mesh's (single) submesh, by the `<source>#materialN` link.
            const std::uint32_t mslot =
                mesh->submeshes.empty() ? 0 : mesh->submeshes[0].material_slot;
            const assets::ManifestEntry* mat_e =
                find_source(*manifest, p.source + "#material" + std::to_string(mslot));
            if (mat_e) {
                if (auto mat = load_material(cooked_dir / mat_e->cooked_file)) {
                    lm.material_asset = *mat;
                    lm.textures = request_textures(*manifest, *mat);
                }
            }
            lm.material = materials.add(build_desc(lm.material_asset, lm.textures, bridge, {}));

            // Lay the models out in a row along X, centered, and spin them in place.
            const float x = (static_cast<float>(slot) - (count - 1) * 0.5f) * 3.0f;
            core::Transform tf{};
            tf.translation = {x, 0.0f, 0.0f};
            lm.entity = app.world().spawn_with(
                WorldTransform{tf},
                render::MeshRef{mesh_id},
                render::MaterialRef{lm.material},
                Turntable{{x, 0.0f, 0.0f}, 0.35f * static_cast<float>(slot), 0.6f});
            std::printf("  loaded '%s': %u tris, material%u%s\n",
                        lm.name.c_str(),
                        lm.triangles,
                        mslot,
                        lm.rigged ? " (posed)" : "");
            models.push_back(std::move(lm));
            ++slot;
        }
    }

    // Request a material's five texture slots through the bridge, resolving each content id to its
    // cooked file via the manifest. A zero id (no map) stays invalid → the shader's 1×1 fallback.
    ModelTextures request_textures(const assets::Manifest& manifest,
                                   const assets::MaterialAsset& m) {
        ModelTextures out;
        const auto req = [&](assets::AssetId id) -> assets::TextureAssetHandle {
            if (!id.is_valid())
                return {};
            const assets::ManifestEntry* e = manifest.find_by_id(id);
            if (!e)
                return {};
            return bridge.request_texture(cooked_dir / e->cooked_file);
        };
        out.base_color = req(m.base_color_tex);
        out.metallic_roughness = req(m.metallic_roughness_tex);
        out.normal = req(m.normal_tex);
        out.occlusion = req(m.occlusion_tex);
        out.emissive = req(m.emissive_tex);
        return out;
    }

    // Main-thread, once per frame: ready any completed CPU loads, upload newly-ready textures, and
    // if anything new landed, refresh the affected materials so their placeholder swaps for the
    // real texture. Cheap and idempotent when nothing is pending.
    void pump_and_drain() {
        server.pump();
        if (bridge.drain() > 0)
            refresh_materials({});
    }

    // Rebuild every model's material desc from its (possibly newly-resident) textures. `strip` lets
    // a headless control render omit one map to isolate its contribution.
    void refresh_materials(Strip strip) {
        for (const auto& m : models)
            materials.update(m.material, build_desc(m.material_asset, m.textures, bridge, strip));
    }

    void pose_camera(float yaw, float pitch, float distance) {
        app.world().get<ecs::WorldTransform>(camera)->value =
            camera_transform(yaw, pitch, distance, {0.0f, 0.6f, 0.0f});
    }

    std::vector<std::uint8_t> readback() {
        return read_rgba8(*app.device(), app.graph()->physical(last_ldr), kWidth, kHeight);
    }

    // Small helpers over the manifest
    // ---------------------------------------------------------------
    static const assets::ManifestEntry* find_source(const assets::Manifest& mf,
                                                    const std::string& s) {
        return mf.find_by_source(s);
    }

    static const assets::ManifestEntry* find_prefix(const assets::Manifest& mf,
                                                    const std::string& prefix) {
        for (const auto& e : mf.entries())
            if (e.source_path.rfind(prefix, 0) == 0)
                return &e;
        return nullptr;
    }

    static std::optional<assets::MaterialAsset> load_material(const std::filesystem::path& p) {
        const auto bytes = platform::read_file(p);
        if (!bytes)
            return std::nullopt;
        assets::AssetError err{};
        return assets::read_material(*bytes, err);
    }

    static std::optional<assets::Skeleton> load_skeleton(const std::filesystem::path& p) {
        const auto bytes = platform::read_file(p);
        if (!bytes)
            return std::nullopt;
        assets::AssetError err{};
        return assets::read_skeleton(*bytes, err);
    }

    static std::optional<assets::Clip> load_clip(const std::filesystem::path& p) {
        const auto bytes = platform::read_file(p);
        if (!bytes)
            return std::nullopt;
        assets::AssetError err{};
        return assets::read_clip(*bytes, err);
    }
};

app::AppConfig gpu_config() {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kWidth, kHeight};
    cfg.tick_hz = 60.0;
    return cfg;
}

// ── Structural proofs (headless) ─────────────────────────────────────────────────────────────────
bool scene_is_lit(const std::vector<std::uint8_t>& px) {
    std::uint64_t lit = 0, bright = 0;
    const std::size_t n = px.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const int lum = (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3;
        if (lum > 25)
            ++lit;
        if (lum > 170)
            ++bright;
    }
    std::printf("  self-check: %llu lit / %llu bright of %zu px\n",
                static_cast<unsigned long long>(lit),
                static_cast<unsigned long long>(bright),
                n);
    return lit > n / 40 && bright > 30;
}

// Count pixels that differ by more than `thresh` in summed channel delta — how much one feature
// moved the image. A cooked texture that reaches the shader changes thousands of pixels; noise
// changes tens.
std::uint64_t
image_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int thresh) {
    std::uint64_t n = 0;
    const std::size_t px = std::min(a.size(), b.size()) / 4;
    for (std::size_t i = 0; i < px; ++i) {
        const int d = std::abs(a[i * 4] - b[i * 4]) + std::abs(a[i * 4 + 1] - b[i * 4 + 1]) +
                      std::abs(a[i * 4 + 2] - b[i * 4 + 2]);
        if (d > thresh)
            ++n;
    }
    return n;
}

// ── --headless: render, self-check, exit code ────────────────────────────────────────────────────
int run_headless(const std::filesystem::path& cooked, int frames, const char* ppm) {
    app::Application app(gpu_config());
    if (!app.device()) {
        std::fprintf(stderr, "08-gltf-zoo: no Vulkan device (need a driver or lavapipe)\n");
        return std::getenv("RIME_REQUIRE_VULKAN") ? 1 : 0; // absent GPU: a skip, unless required
    }
    Zoo zoo(app, cooked);
    if (zoo.models.empty()) {
        std::fprintf(stderr, "08-gltf-zoo: no models loaded (is the cook fixture wired?)\n");
        return 1;
    }
    std::printf("08-gltf-zoo: %zu model(s) on '%s' (%ux%u)\n",
                zoo.models.size(),
                app.device()->adapter().name.c_str(),
                kWidth,
                kHeight);

    // Block for the textures too, so the proof sees the real cooked maps (not placeholders), then
    // spin the turntable a few frames to a settled pose.
    zoo.server.wait_for_pending_loads();
    zoo.pump_and_drain();
    zoo.pose_camera(0.5f, 0.34f, 9.5f);
    for (int i = 0; i < frames; ++i)
        zoo.app.step(zoo.app.fixed_dt());

    // (A) the real scene; (C) base-color textures stripped; (D) normal maps stripped. Each control
    // isolates one cooked-texture feature's contribution to the shaded image.
    zoo.refresh_materials({});
    zoo.app.step(zoo.app.fixed_dt());
    const std::vector<std::uint8_t> a = zoo.readback();

    zoo.refresh_materials(Strip{true, false});
    zoo.app.step(zoo.app.fixed_dt());
    const std::vector<std::uint8_t> c = zoo.readback();

    zoo.refresh_materials(Strip{false, true});
    zoo.app.step(zoo.app.fixed_dt());
    const std::vector<std::uint8_t> d = zoo.readback();

    zoo.refresh_materials({}); // restore

    const bool lit = scene_is_lit(a);
    const std::uint64_t base_tex = image_diff(a, c, 24);
    const std::uint64_t normal_map = image_diff(a, d, 16);
    bool rig_ok = true;
    std::size_t rigged = 0;
    for (const auto& m : zoo.models)
        if (m.rigged)
            ++rigged;
    rig_ok = rigged > 0; // at least one model exercised the AN0 posed path

    std::printf("  base-color texture reaches shading: %llu px changed when stripped\n",
                static_cast<unsigned long long>(base_tex));
    std::printf("  normal map perturbs shading:        %llu px changed when stripped\n",
                static_cast<unsigned long long>(normal_map));
    std::printf("  textures uploaded to GPU: %zu · rigged models posed: %zu\n",
                zoo.bridge.uploaded_count(),
                rigged);
    if (ppm)
        write_ppm(ppm, a, kWidth, kHeight);

    const bool ok =
        lit && base_tex > 2000 && normal_map > 1500 && rig_ok && zoo.bridge.uploaded_count() > 0;
    std::printf("08-gltf-zoo: %s\n",
                ok ? "the zoo lives — import→cook→load→render green!" : "FAILED self-check");
    return ok ? 0 : 1;
}

// ── --serve: stream the gallery live and let the client steer (Track S0) ─────────────────────────
std::uint64_t now_us() {
    return platform::Clock::now_ns() / 1000;
}

void apply_input(OrbitView& v, const stream::InputEvent& e) {
    using K = stream::InputEvent::Kind;
    switch (e.kind) {
        case K::PointerMove:
            v.yaw.store((static_cast<float>(e.x) / static_cast<float>(kWidth)) * 6.2831853f);
            v.pitch.store(
                std::clamp((0.5f - static_cast<float>(e.y) / static_cast<float>(kHeight)) * 2.4f,
                           -1.4f,
                           1.4f));
            break;
        case K::PointerScroll:
            v.distance.store(std::clamp(v.distance.load() - e.scroll_y * 0.8f, 4.0f, 24.0f));
            break;
        case K::KeyDown:
            v.yaw.store(0.5f);
            v.pitch.store(0.30f);
            v.distance.store(9.5f);
            break;
        default:
            break;
    }
}

int run_serve(const std::filesystem::path& cooked,
              const std::string& host,
              std::uint16_t port,
              stream::Codec codec,
              int slow_io_ms) {
    app::Application app(gpu_config());
    if (!app.device()) {
        std::fprintf(stderr, "08-gltf-zoo server: no Vulkan device (need lavapipe/a GPU)\n");
        return 1;
    }
    Zoo zoo(app, cooked);
    if (zoo.models.empty()) {
        std::fprintf(stderr, "08-gltf-zoo server: no models loaded (cook the assets first)\n");
        return 1;
    }
    // Without --slow-io, resolve textures up front (instant on a fast disk). With it, DON'T: hold
    // off draining so the first frames show the magenta placeholder, then let it swap in — the M6.5
    // async story, made watchable. The loads run on the job system regardless; we simply time the
    // GPU pop-in.
    if (slow_io_ms <= 0) {
        zoo.server.wait_for_pending_loads();
        zoo.pump_and_drain();
    }
    auto streamer = stream::FrameStreamer::create(*app.device(), {kWidth, kHeight});
    auto listener = platform::TcpListener::bind(port, host);
    if (!streamer || !listener) {
        std::fprintf(stderr, "08-gltf-zoo server: could not create streamer/listener\n");
        return 1;
    }
    std::printf(
        "08-gltf-zoo server: %zu models on '%s'; listening on %s:%u — waiting for a client…\n",
        zoo.models.size(),
        app.device()->adapter().name.c_str(),
        host.c_str(),
        listener->local_port());
    auto accepted = listener->accept();
    if (!accepted) {
        std::fprintf(stderr, "08-gltf-zoo server: accept failed\n");
        return 1;
    }
    stream::ProtocolConnection conn(std::move(*accepted));
    if (!conn.handshake()) {
        std::fprintf(stderr, "08-gltf-zoo server: handshake failed\n");
        return 1;
    }
    std::printf("08-gltf-zoo server: client connected — streaming 720p.\n");

    OrbitView view;
    std::atomic<bool> stop{false};
    std::thread input_thread([&] {
        stream::MessageType type{};
        std::vector<std::byte> payload;
        while (!stop.load()) {
            if (!conn.recv_message(type, payload) || type == stream::MessageType::Bye)
                break;
            if (type == stream::MessageType::Input) {
                stream::InputEvent e;
                if (e.decode(payload))
                    apply_input(view, e);
            }
        }
        stop.store(true);
    });

    stream::FrameEncoder encoder;
    std::uint64_t seq = 0;
    std::uint64_t last_ns = platform::Clock::now_ns();
    const std::uint64_t start_ns = last_ns;
    const auto period = std::chrono::milliseconds(33);
    auto next = std::chrono::steady_clock::now();
    while (!stop.load()) {
        const std::uint64_t now_ns = platform::Clock::now_ns();
        const double dt = static_cast<double>(now_ns - last_ns) * 1e-9;
        last_ns = now_ns;

        // Slow-io demo: withhold the GPU upload for the warm-up window, then let the placeholders
        // resolve. pump_and_drain() is a no-op once everything is resident.
        if (slow_io_ms > 0 && static_cast<int>((now_ns - start_ns) / 1'000'000) >= slow_io_ms)
            zoo.pump_and_drain();
        else if (slow_io_ms <= 0)
            zoo.pump_and_drain();

        zoo.pose_camera(view.yaw.load(), view.pitch.load(), view.distance.load());
        zoo.app.step(dt);

        const stream::FrameView fv = streamer->capture(zoo.app.graph()->physical(zoo.last_ldr));
        stream::FrameMessage fm;
        fm.sequence = seq;
        fm.capture_us = now_us();
        fm.codec = codec;
        fm.desc = {{kWidth, kHeight}, fv.format};
        if (!encoder.encode(codec, fm.desc, fv.pixels, fm.data)) {
            std::fprintf(stderr, "08-gltf-zoo server: encode failed\n");
            break;
        }
        if (!conn.send_frame(fm)) {
            std::printf("08-gltf-zoo server: client disconnected after %llu frames.\n",
                        static_cast<unsigned long long>(seq));
            break;
        }
        ++seq;
        next += period;
        std::this_thread::sleep_until(next);
    }
    stop.store(true);
    input_thread.join();
    std::printf("08-gltf-zoo server: done (streamed %llu frames, avg capture %.2f ms).\n",
                static_cast<unsigned long long>(seq),
                streamer->stats().avg_ms);
    return 0;
}

stream::Codec parse_codec(std::string_view s) {
    if (s == "raw")
        return stream::Codec::Raw;
    if (s == "lz4")
        return stream::Codec::LZ4;
    return stream::Codec::Jpeg;
}

} // namespace

int main(int argc, char** argv) {
    enum class Mode { Headless, Serve, Windowed } mode = Mode::Headless;
    int frames = 30;
    int slow_io_ms = 0;
    const char* ppm = nullptr;
    std::filesystem::path cooked = RIME_ZOO_COOKED_DIR;
    std::string host = "0.0.0.0";
    std::uint16_t port = 9100;
    stream::Codec codec = stream::Codec::Jpeg;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--headless")
            mode = Mode::Headless;
        else if (a == "--serve")
            mode = Mode::Serve;
        else if (a == "--windowed")
            mode = Mode::Windowed;
        else if (a == "--cooked" && i + 1 < argc)
            cooked = argv[++i];
        else if (a == "--frames" && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (a == "--slow-io" && i + 1 < argc)
            slow_io_ms = std::atoi(argv[++i]);
        else if (a == "--ppm" && i + 1 < argc)
            ppm = argv[++i];
        else if (a == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--codec" && i + 1 < argc)
            codec = parse_codec(argv[++i]);
    }

    if (mode == Mode::Windowed) {
        std::printf("08-gltf-zoo: --windowed needs a display (Mac); running --headless instead.\n");
        mode = Mode::Headless;
    }
    if (mode == Mode::Serve)
        return run_serve(cooked, host, port, codec, slow_io_ms);
    return run_headless(cooked, frames, ppm);
}
