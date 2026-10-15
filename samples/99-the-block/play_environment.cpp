// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_environment.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "rime/core/math.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"

namespace rime::block_play {
namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr unsigned kTexels = 256;

// Integer hashing keeps the authored placement reproducible; no clock or process-global RNG.
std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

float random(std::uint32_t x) {
    return static_cast<float>(hash(x) & 0xffffu) / 65535.0f;
}

float noise(float x, float y, int period) {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    auto at = [&](int a, int b) {
        a = (a % period + period) % period;
        b = (b % period + period) % period;
        return random(static_cast<std::uint32_t>(a) * 65537u + static_cast<std::uint32_t>(b));
    };
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const float u = fx * fx * (3.0f - 2.0f * fx), v = fy * fy * (3.0f - 2.0f * fy);
    return std::lerp(std::lerp(at(ix, iy), at(ix + 1, iy), u),
                     std::lerp(at(ix, iy + 1), at(ix + 1, iy + 1), u),
                     v);
}

std::uint8_t byte(float x) {
    return static_cast<std::uint8_t>(std::clamp(x * 255.0f + .5f, 0.0f, 255.0f));
}

float srgb(float x) {
    return x <= .0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - .055f;
}

enum class Surface { Brick, Plaster, Concrete, Bark, Grass, Dirt, Leaf, Water };

struct Texel {
    core::Vec3 color{1, 1, 1};
    float height = .5f;
    float roughness = .9f;
    float alpha = 1;
};

Texel surface(Surface kind, float u, float v) {
    const float fine = noise(u * 128, v * 128, 128);
    const float medium = noise(u * 16, v * 16, 16);
    const float broad = noise(u * 4, v * 4, 4);
    Texel t;
    switch (kind) {
        case Surface::Brick: {
            // Four stretchers by twelve courses per metre, alternating half-brick bond.
            const int row = static_cast<int>(v * 12);
            const float x = u * 4 + (row % 2) * .5f;
            const float bx = x - std::floor(x), by = v * 12 - std::floor(v * 12);
            const float edge = std::min(std::min(bx, 1 - bx) * .25f, std::min(by, 1 - by) / 12.0f);
            const float brick = std::clamp((edge - .002f) / .004f, 0.0f, 1.0f);
            const float tone = .76f + .28f * noise(std::floor(x), static_cast<float>(row), 12);
            t.color = core::Vec3{.72f, .64f, .51f} * (1 - brick) +
                      core::Vec3{tone, tone * .94f, tone * .87f} * brick;
            t.color = t.color * (.88f + fine * .12f + medium * .08f);
            t.height = .18f + brick * (.40f + fine * .09f);
            t.roughness = .78f + .20f * fine;
            break;
        }
        case Surface::Plaster: {
            const float staining = .72f + broad * .20f + medium * .08f;
            const float chips = std::clamp((medium - .70f) * 5, 0.0f, 1.0f);
            t.color = core::Vec3{1.0f, .98f, .92f} * staining * (1 - chips * .23f);
            t.height = .44f + fine * .1f - chips * .14f;
            t.roughness = .77f + fine * .2f;
            break;
        }
        case Surface::Concrete:
            t.color = core::Vec3{.83f, .84f, .81f} * (.76f + medium * .17f + fine * .12f);
            t.height = .38f + fine * .15f + medium * .04f;
            t.roughness = .8f + fine * .18f;
            break;
        case Surface::Bark: {
            const float groove =
                std::pow(.5f + .5f * std::sin(u * kPi * 32 + noise(u * 8, v * 4, 8) * 5), 4);
            t.color = core::Vec3{.15f, .085f, .041f} * (.6f + medium * .5f - groove * .3f);
            t.height = .58f - groove * .22f + fine * .12f;
            break;
        }
        case Surface::Grass:
            t.color = core::Vec3{.095f, .135f, .038f} * (.6f + broad * .4f + fine * .65f);
            t.height = .3f + fine * .5f;
            break;
        case Surface::Dirt: {
            const float stone = std::clamp((fine - .65f) * 4, 0.0f, 1.0f);
            t.color = core::Vec3{.135f, .085f, .043f} * (.65f + medium * .55f) +
                      core::Vec3{.05f, .045f, .035f} * stone;
            t.height = .3f + fine * .35f;
            break;
        }
        case Surface::Leaf: {
            // A pointed oval with scalloped margin and a central vein. Transparent surroundings
            // survive in both depth and shadow passes through the existing alpha-mask material.
            const float y = v * 2 - 1;
            const float width = .74f * std::pow(std::max(0.0f, 1 - y * y), .7f);
            const float edge = width - std::abs(u * 2 - 1);
            t.alpha = edge > .02f + .018f * std::sin(v * kPi * 26) ? 1.0f : 0.0f;
            const float vein = std::exp(-std::abs(u - .5f) * 110);
            t.color = core::Vec3{.055f, .12f, .022f} * (.75f + medium * .5f) +
                      core::Vec3{.024f, .025f, .005f} * vein;
            t.height = .5f + vein * .04f;
            break;
        }
        case Surface::Water:
            t.color = {.018f, .029f, .026f};
            t.height = .5f + .012f * std::sin(u * kPi * 10 + std::sin(v * kPi * 4)) +
                       .008f * std::sin((u + v) * kPi * 18);
            t.roughness = .075f + medium * .035f;
            break;
    }
    return t;
}

struct Maps {
    rhi::TextureHandle albedo{}, normal{}, material{};
};

Maps make_maps(rhi::Device& device, std::vector<rhi::TextureHandle>& owned, Surface kind) {
    std::vector<Texel> texels(kTexels * kTexels);
    for (unsigned y = 0; y < kTexels; ++y)
        for (unsigned x = 0; x < kTexels; ++x)
            texels[y * kTexels + x] =
                surface(kind, static_cast<float>(x) / kTexels, static_cast<float>(y) / kTexels);
    std::array<std::vector<std::uint8_t>, 3> pixels;
    for (auto& p : pixels)
        p.resize(kTexels * kTexels * 4);
    auto height = [&](unsigned x, unsigned y) {
        return texels[(y % kTexels) * kTexels + x % kTexels].height;
    };
    for (unsigned y = 0; y < kTexels; ++y) {
        for (unsigned x = 0; x < kTexels; ++x) {
            const auto& t = texels[y * kTexels + x];
            const unsigned i = (y * kTexels + x) * 4;
            pixels[0][i] = byte(srgb(t.color.x));
            pixels[0][i + 1] = byte(srgb(t.color.y));
            pixels[0][i + 2] = byte(srgb(t.color.z));
            // Terrain interprets A as height. Only leaf cards need alpha for coverage.
            pixels[0][i + 3] = byte(kind == Surface::Leaf ? t.alpha : t.height);
            const float strength = kind == Surface::Water ? .45f : 2.2f;
            const core::Vec3 n = core::normalize(
                core::Vec3{(height(x + kTexels - 1, y) - height(x + 1, y)) * strength,
                           (height(x, y + kTexels - 1) - height(x, y + 1)) * strength,
                           1});
            pixels[1][i] = byte(n.x * .5f + .5f);
            pixels[1][i + 1] = byte(n.y * .5f + .5f);
            pixels[1][i + 2] = byte(n.z * .5f + .5f);
            pixels[1][i + 3] = 255;
            pixels[2][i] = 255;
            pixels[2][i + 1] = byte(t.roughness);
            pixels[2][i + 2] = 0;
            pixels[2][i + 3] = 255;
        }
    }
    Maps maps;
    const std::array<rhi::TextureHandle*, 3> handles{&maps.albedo, &maps.normal, &maps.material};
    for (unsigned k = 0; k < 3; ++k) {
        rhi::TextureDesc d{};
        d.extent = {kTexels, kTexels};
        d.mip_levels = 9;
        d.format = k == 0 ? rhi::Format::RGBA8Srgb : rhi::Format::RGBA8Unorm;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        d.debug_name = "play-environment-map";
        *handles[k] = device.create_texture(d);
        if (handles[k]->is_valid()) {
            device.write_texture(*handles[k], pixels[k].data(), pixels[k].size());
            owned.push_back(*handles[k]);
        }
    }
    return maps;
}

render::PbrMaterialDesc material(const Maps& maps, core::Vec3 tint = {1, 1, 1}) {
    render::PbrMaterialDesc d;
    d.base_color[0] = tint.x;
    d.base_color[1] = tint.y;
    d.base_color[2] = tint.z;
    d.roughness = 1;
    d.base_color_texture = maps.albedo;
    d.normal_texture = maps.normal;
    d.metallic_roughness_texture = maps.material;
    return d;
}

void quad(render::CpuMesh& mesh, core::Vec3 c, core::Vec3 u, core::Vec3 v) {
    const core::Vec3 n = core::normalize(core::cross(u, v));
    const auto i = static_cast<std::uint32_t>(mesh.vertices.size());
    const std::array<core::Vec3, 4> p{c - u - v, c + u - v, c + u + v, c - u + v};
    for (unsigned k = 0; k < 4; ++k) {
        render::MeshVertex a;
        a.px = p[k].x;
        a.py = p[k].y;
        a.pz = p[k].z;
        a.nx = n.x;
        a.ny = n.y;
        a.nz = n.z;
        a.u = k == 1 || k == 2 ? 1.0f : 0.0f;
        a.v = k >= 2 ? 1.0f : 0.0f;
        mesh.vertices.push_back(a);
    }
    mesh.indices.insert(mesh.indices.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
}

void add_back_faces(render::CpuMesh& mesh) {
    // These thin leaves/blades need a shading normal facing EACH visible side. Disabling
    // culling alone leaves an underside using its top-side normal; NdotV then clamps near zero
    // and the reflection becomes white at every angle below the canopy. Explicit back faces
    // preserve correct culling, opposite normals and tangent frames without a new shader mode.
    const auto vertices = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto indices = mesh.indices.size();
    mesh.vertices.reserve(vertices * 2);
    mesh.indices.reserve(indices * 2);
    for (std::uint32_t i = 0; i < vertices; ++i) {
        auto v = mesh.vertices[i];
        v.nx = -v.nx;
        v.ny = -v.ny;
        v.nz = -v.nz;
        mesh.vertices.push_back(v);
    }
    for (std::size_t i = 0; i < indices; i += 3) {
        const auto a = mesh.indices[i] + vertices;
        const auto b = mesh.indices[i + 1] + vertices;
        const auto c = mesh.indices[i + 2] + vertices;
        mesh.indices.insert(mesh.indices.end(), {a, c, b});
    }
}

void branch(render::CpuMesh& mesh, core::Vec3 from, core::Vec3 to, float radius) {
    const core::Vec3 axis = core::normalize(to - from);
    const core::Vec3 u = core::normalize(
        core::cross(axis, std::abs(axis.y) < .9f ? core::Vec3{0, 1, 0} : core::Vec3{1, 0, 0}));
    const core::Vec3 v = core::cross(axis, u);
    for (unsigned k = 0; k < 10; ++k) {
        const auto i = static_cast<std::uint32_t>(mesh.vertices.size());
        for (unsigned j = 0; j < 4; ++j) {
            const float a = 2 * kPi * static_cast<float>(k + (j == 1 || j == 2 ? 1 : 0)) / 10;
            const bool top = j >= 2;
            const core::Vec3 n = u * std::cos(a) + v * std::sin(a);
            const core::Vec3 p = (top ? to : from) + n * radius * (top ? .65f : 1);
            render::MeshVertex vert;
            vert.px = p.x;
            vert.py = p.y;
            vert.pz = p.z;
            vert.nx = n.x;
            vert.ny = n.y;
            vert.nz = n.z;
            vert.u = static_cast<float>(k + (j == 1 || j == 2 ? 1 : 0)) / 10;
            vert.v = top ? core::length(to - from) * .7f : 0;
            mesh.vertices.push_back(vert);
        }
        mesh.indices.insert(mesh.indices.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
    }
}

std::array<render::CpuMesh, 2> tree_meshes() {
    std::array<render::CpuMesh, 2> m;
    branch(m[0], {}, {.08f, 4.8f, .1f}, .22f);
    for (unsigned b = 0; b < 9; ++b) {
        const float angle = static_cast<float>(b) * 2.4f;
        const core::Vec3 center{std::cos(angle) * (1.1f + random(b) * .5f),
                                3.5f + random(b + 90) * 2.2f,
                                std::sin(angle) * (1.1f + random(b) * .5f)};
        branch(m[0], {0, 2.1f + static_cast<float>(b) * .23f, 0}, center, .075f);
        for (unsigned l = 0; l < 100; ++l) {
            const unsigned seed = b * 1000 + l * 7;
            const float yaw = random(seed + 3) * kPi * 2;
            const float pitch = (random(seed + 4) - .5f) * 1.8f;
            const core::Vec3 at = center + core::Vec3{(random(seed) - .5f) * 2.7f,
                                                      (random(seed + 1) - .5f) * 1.8f,
                                                      (random(seed + 2) - .5f) * 2.7f};
            const float size = .16f + random(seed + 5) * .22f;
            quad(m[1],
                 at,
                 {std::cos(yaw) * size, 0, -std::sin(yaw) * size},
                 {-std::sin(yaw) * std::cos(pitch) * size,
                  std::sin(pitch) * size,
                  -std::cos(yaw) * std::cos(pitch) * size});
        }
    }
    add_back_faces(m[1]);
    for (auto& mesh : m)
        render::compute_tangents(mesh);
    return m;
}

render::CpuMesh ground_patch(float half_x, float half_z) {
    auto mesh = render::make_plane(1);
    for (auto& v : mesh.vertices) {
        v.px *= half_x;
        v.pz *= half_z;
        v.u = v.px * .5f;
        v.v = v.pz * .5f;
    }
    render::compute_tangents(mesh);
    return mesh;
}

render::CpuMesh water_patch(float half_x, float half_z, unsigned seed) {
    // A shallow, irregular wet outline, with a fine ripple normal rather than a flat mirror.
    // Water is opaque deep-water shading for this brick: SSR handles on-screen objects, and
    // the existing filtered sky handles misses. Refraction/transparency are separate features.
    render::CpuMesh m;
    m.vertices.push_back({0, 0, 0, 0, 1, 0, 0, 0});
    constexpr unsigned kSides = 64;
    for (unsigned i = 0; i < kSides; ++i) {
        const float a = 2 * kPi * static_cast<float>(i) / kSides;
        const float radius = .88f + .07f * std::sin(a * 3 + seed) + .05f * std::sin(a * 7);
        const float x = std::cos(a) * half_x * radius, z = std::sin(a) * half_z * radius;
        m.vertices.push_back({x, 0, z, 0, 1, 0, x * .2f, z * .2f});
        m.indices.insert(m.indices.end(), {0, (i + 1) % kSides + 1, i + 1});
    }
    render::compute_tangents(m);
    return m;
}

void spawn(ecs::World& world,
           render::MeshId mesh,
           render::MaterialId mat,
           core::Vec3 at,
           float scale = 1,
           float yaw = 0) {
    core::Transform tf;
    tf.translation = at;
    tf.scale = {scale, scale, scale};
    tf.rotation = core::quat_from_axis_angle({0, 1, 0}, yaw);
    (void)world.spawn_with(ecs::LocalTransform{tf},
                           ecs::WorldTransform{tf},
                           render::MeshRef{mesh},
                           render::MaterialRef{mat});
}

float ground_y(const assets::ProceduralTerrainConfig& c, int x, int z) {
    if (x >= c.street_min_x && x <= c.street_max_x && z >= c.street_min_z && z <= c.street_max_z)
        return 0;
    const auto q = assets::procedural_terrain_sample(c, x, z);
    return q ? assets::kProceduralHeightOffset + *q * assets::kProceduralHeightScale : 0;
}
} // namespace

PlayEnvironment::~PlayEnvironment() {
    device_.wait_idle();
    for (const auto t : textures_)
        device_.destroy(t);
}

bool PlayEnvironment::build(ecs::World& world,
                            render::MeshRegistry& meshes,
                            render::MaterialRegistry& materials,
                            const blockkit::BlockPalette& block,
                            const assets::ProceduralTerrainConfig& config) {
    if (!textures_.empty())
        return false; // one art build per scene
    std::array<Maps, 8> maps;
    for (unsigned i = 0; i < maps.size(); ++i) {
        maps[i] = make_maps(device_, textures_, static_cast<Surface>(i));
        if (!maps[i].albedo.is_valid() || !maps[i].normal.is_valid() ||
            !maps[i].material.is_valid())
            return false;
    }
    auto get = [&](Surface s) -> const Maps& { return maps[static_cast<unsigned>(s)]; };
    for (unsigned t = 0; t < blockkit::kTintCount; ++t) {
        const Surface kind = t == 1   ? Surface::Brick
                             : t == 0 ? Surface::Concrete
                                      : Surface::Plaster;
        for (auto id : {block.facade[t], block.band[t]}) {
            auto d = materials.get(id);
            d.base_color_texture = get(kind).albedo;
            d.normal_texture = get(kind).normal;
            d.metallic_roughness_texture = get(kind).material;
            d.roughness = 1;
            materials.update(id, d);
            ++stats_.wall_materials;
        }
    }
    for (auto id : {block.roof, block.kerb, block.barrier}) {
        auto d = materials.get(id);
        d.base_color_texture = get(Surface::Concrete).albedo;
        d.normal_texture = get(Surface::Concrete).normal;
        materials.update(id, d);
    }
    const auto bark = materials.add(material(get(Surface::Bark)));
    auto leaf_desc = material(get(Surface::Leaf));
    leaf_desc.alpha_cutoff = .5f;
    const auto leaf = materials.add(leaf_desc);
    const auto grass = materials.add(material(get(Surface::Grass)));
    const auto dirt = materials.add(material(get(Surface::Dirt)));
    const auto water = materials.add(material(get(Surface::Water)));
    water_ = water;

    const auto tree = tree_meshes();
    const auto trunk_mesh = meshes.add(tree[0], "play-tree-branches");
    const auto leaf_mesh = meshes.add(tree[1], "play-tree-leaves");
    if (!meshes.contains(trunk_mesh) || !meshes.contains(leaf_mesh))
        return false;
    constexpr std::array<core::Vec3, 26> trees{
        {{-8, 0, -7},   {-8, 0, 8},   {10, 0, -8},  {22, 0, 8},   {34, 0, -8},  {46, 0, 8},
         {-10, 0, 20},  {0, 0, 18},   {12, 0, 20},  {24, 0, 18},  {38, 0, 22},  {50, 0, 20},
         {-10, 0, -20}, {4, 0, -18},  {18, 0, -20}, {30, 0, -18}, {44, 0, -20}, {54, 0, -22},
         {64, 0, 14},   {68, 0, -16}, {80, 0, 22},  {92, 0, -26}, {10, 0, 48},  {28, 0, 52},
         {42, 0, -50},  {-22, 0, -48}}};
    for (unsigned i = 0; i < trees.size(); ++i) {
        auto p = trees[i];
        p.y = ground_y(config, static_cast<int>(p.x), static_cast<int>(p.z));
        const float size = .85f + random(i + 200) * .45f;
        const float yaw = random(i + 400) * kPi * 2;
        spawn(world, trunk_mesh, bark, p, size, yaw);
        spawn(world, leaf_mesh, leaf, p, size, yaw);
        ++stats_.trees;
    }

    // The original slab extends far behind the houses. Dress its unused outer strips as verges,
    // keeping the same physical surface at y=0. Beyond the slab, actual streamed tiles take over.
    const auto verge = meshes.add(ground_patch(38, 11.8f), "play-grass-verge");
    spawn(world, verge, grass, {22, .008f, -26.2f});
    spawn(world, verge, grass, {22, .008f, 26.2f});
    const auto path = meshes.add(ground_patch(38, .85f), "play-dirt-path");
    spawn(world, path, dirt, {22, .012f, -16.0f});
    spawn(world, path, dirt, {22, .012f, 16.0f});

    // Tufts are geometry, not a painted green floor: crossed tapered blades with true silhouettes.
    // Merge all nearby tufts into four patches so grass does not add a draw call per blade.
    auto grass_desc = material(get(Surface::Grass));
    const auto blades_mat = materials.add(grass_desc);
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned section = 0; section < 2; ++section) {
            render::CpuMesh blades;
            for (unsigned i = 0; i < 900; ++i) {
                const unsigned seed = 10000 + side * 10000 + section * 2000 + i * 7;
                const float x = -15 + static_cast<float>(section) * 38 + random(seed) * 38;
                const float z = (side == 0 ? -1.0f : 1.0f) * (14.5f + random(seed + 1) * 21.5f);
                const float pond_x = (x - 28) / 7.8f, pond_z = (z - 26) / 5.4f;
                if (pond_x * pond_x + pond_z * pond_z < 1)
                    continue;
                for (unsigned b = 0; b < 3; ++b) {
                    const float a = random(seed + 2 + b) * 2 * kPi;
                    const float h = .16f + random(seed + 5) * .25f;
                    const core::Vec3 base{x, .015f, z},
                        width{std::cos(a) * .025f, 0, std::sin(a) * .025f};
                    const core::Vec3 tip =
                        base + core::Vec3{std::sin(a) * .1f, h, std::cos(a) * .1f};
                    const auto index = static_cast<std::uint32_t>(blades.vertices.size());
                    const core::Vec3 n = core::normalize(core::cross(width, tip - base));
                    for (const auto p : {base - width, base + width, tip})
                        blades.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, p.x, p.z});
                    blades.indices.insert(blades.indices.end(), {index, index + 1, index + 2});
                }
                ++stats_.grass_tufts;
            }
            add_back_faces(blades);
            render::compute_tangents(blades);
            spawn(world, meshes.add(blades, "play-grass-blades"), blades_mat, {});
        }
    }
    constexpr std::array<core::Vec3, 7> puddles{{{1, .022f, -2.6f},
                                                 {9, .023f, 2.7f},
                                                 {17, .024f, -3.8f},
                                                 {26, .025f, .9f},
                                                 {35, .024f, -2.1f},
                                                 {42, .023f, 3.4f},
                                                 {-3, .022f, 1.8f}}};
    for (unsigned i = 0; i < puddles.size(); ++i) {
        const auto m = meshes.add(
            water_patch(1.3f + random(i) * 1.5f, .55f + random(i + 7) * .6f, i), "play-puddle");
        spawn(world, m, water, puddles[i], 1, random(i + 15) * kPi);
        ++stats_.puddles;
    }
    // A shallow park pool sits on the flat authored surface. Its dirt rim and plants make the
    // shoreline legible without changing the shared collision heightfield or creating a hole.
    const auto bank = meshes.add(water_patch(7.6f, 5.2f, 13), "play-pond-bank");
    const auto pond = meshes.add(water_patch(6.9f, 4.5f, 13), "play-pond-water");
    spawn(world, bank, dirt, {28, .018f, 26});
    spawn(world, pond, water, {28, .035f, 26});
    ++stats_.ponds;

    for (unsigned i = 0; i < terrain_.size(); ++i) {
        const auto s = i == 0 ? Surface::Grass : i == 1 ? Surface::Dirt : Surface::Concrete;
        terrain_[i].base_color = {1, 1, 1};
        terrain_[i].roughness = .92f;
        terrain_[i].albedo_height = get(s).albedo;
        terrain_[i].uv_scale[0] = 2;
        terrain_[i].uv_scale[1] = 2;
        terrain_[i].height_contrast = 1;
    }
    stats_.textures = static_cast<unsigned>(textures_.size());
    return true;
}

void PlayEnvironment::paint_terrain(assets::HeightfieldAsset& a,
                                    const assets::ProceduralTerrainConfig& c) {
    a.weight_columns = a.columns;
    a.weight_rows = a.rows;
    a.layers[0] = assets::AssetId{1};
    a.layers[1] = assets::AssetId{2};
    a.layers[2] = assets::AssetId{3};
    a.weights.resize(static_cast<std::size_t>(a.weight_columns) * a.weight_rows * 4);
    for (unsigned z = 0; z < a.rows; ++z) {
        for (unsigned x = 0; x < a.columns; ++x) {
            const float wx = a.origin.x + static_cast<float>(x) * a.cell_size_x;
            const float wz = a.origin.z + static_cast<float>(z) * a.cell_size_z;
            const float dx = std::max({static_cast<float>(c.street_min_x) - wx,
                                       wx - static_cast<float>(c.street_max_x),
                                       0.0f});
            const float dz = std::max({static_cast<float>(c.street_min_z) - wz,
                                       wz - static_cast<float>(c.street_max_z),
                                       0.0f});
            const float distance = std::sqrt(dx * dx + dz * dz);
            const float patches = noise(wx * .037f, wz * .037f, 256);
            const float dirt = std::clamp(.55f - distance * .025f + patches * .3f, .08f, .8f);
            const auto w = byte(dirt);
            const auto i = (static_cast<std::size_t>(z) * a.columns + x) * 4;
            a.weights[i] = static_cast<std::uint8_t>(255 - w);
            a.weights[i + 1] = w;
            a.weights[i + 2] = 0;
            a.weights[i + 3] = 0;
        }
    }
}
} // namespace rime::block_play
